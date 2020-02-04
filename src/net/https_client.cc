// Copyright 2014 Toggl Desktop developers.

#include "https_client.h"

#include <json/json.h>

#include <string>
#include <sstream>

#include "netconf.h"
#include "urls.h"
#include "toggl_api.h"
#include "util/formatter.h"
#include "util/error.h"

#include <Poco/DeflatingStream.h>
#include <Poco/Environment.h>
#include <Poco/Exception.h>
#include <Poco/FileStream.h>
#include <Poco/InflatingStream.h>
#include <Poco/Logger.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPBasicCredentials.h>
#include <Poco/Net/HTTPCredentials.h>
#include <Poco/Net/HTTPMessage.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/NameValueCollection.h>
#include <Poco/Net/PrivateKeyPassphraseHandler.h>
#include <Poco/Net/SecureStreamSocket.h>
#include <Poco/Net/Session.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/NumberParser.h>
#include <Poco/StreamCopier.h>
#include <Poco/TextEncoding.h>
#include <Poco/URI.h>
#include <Poco/UTF8Encoding.h>

namespace toggl {

void ServerStatus::startStatusCheck() {
    logger().debug("startStatusCheck fast_retry=", fast_retry_);

    if (checker_.isRunning()) {
        return;
    }
    checker_.start();
}

void ServerStatus::stopStatusCheck(const std::string &reason) {
    if (!checker_.isRunning() || checker_.isStopped()) {
        return;
    }

    logger().debug("stopStatusCheck, because ", reason);

    checker_.stop();
    checker_.wait();
}

Logger ServerStatus::logger() const {
    return { "ServerStatus" };
}

void ServerStatus::runActivity() {
    int delay_seconds = 60*3;
    if (!fast_retry_) {
        delay_seconds = 60*15;
    }

    logger().debug( "runActivity loop starting, delay_seconds=", delay_seconds);

    while (!checker_.isStopped()) {
        logger().debug("runActivity delay_seconds=", delay_seconds);

        // Sleep a bit
        for (int i = 0; i < delay_seconds; i++) {
            if (checker_.isStopped()) {
                return;
            }
            Poco::Thread::sleep(1000);
            if (checker_.isStopped()) {
                return;
            }
        }

        // Check server status
        HTTPSClient client;
        HTTPSRequest req;
        req.host = urls::API();
        req.relative_url = "/api/v9/status";

        HTTPSResponse resp = client.Get(req);
        if (noError != resp.err) {
            logger().error(resp.err);

            srand(static_cast<unsigned>(time(nullptr)));
            float low(1.0), high(1.5);
            if (!fast_retry_) {
                low = 1.5;
                high = 2.0;
            }
            float r = low + static_cast<float>(rand()) /
                      (static_cast<float>(RAND_MAX / (high - low)));
            delay_seconds = static_cast<int>(delay_seconds * r);

            logger().debug("err=", resp.err, ", random=", r, ", delay_seconds=", delay_seconds);

            continue;
        }

        stopStatusCheck("No error from backend");
    }
}

error ServerStatus::Status() {
    if (gone_) {
        return error::kEndpointGoneError;
    }
    if (checker_.isRunning() && !checker_.isStopped()) {
        return error::kBackendIsDownError;
    }
    return noError;
}

void ServerStatus::UpdateStatus(const Poco::Int64 code) {
    logger().debug("UpdateStatus status_code=", code);

    gone_ = 410 == code;

    if (code >= 500 && code < 600) {
        fast_retry_ = 500 != code;
        startStatusCheck();
        return;
    }

    stopStatusCheck("Status code " + std::to_string(code));
}

HTTPSClientConfig HTTPSClient::Config;
std::map<std::string, Poco::Timestamp> HTTPSClient::banned_until_;

Logger HTTPSClient::logger() const {
    return { "HTTPSClient" };
}

bool HTTPSClient::isRedirect(const Poco::Int64 status_code) const {
    return (status_code >= 300 && status_code < 400);
}

HTTPSResponse HTTPSClient::Post(
    HTTPSRequest req) const {
    req.method = Poco::Net::HTTPRequest::HTTP_POST;
    return request(req);
}

HTTPSResponse HTTPSClient::Get(
    HTTPSRequest req) const {
    req.method = Poco::Net::HTTPRequest::HTTP_GET;
    return request(req);
}

HTTPSResponse HTTPSClient::GetFile(
    HTTPSRequest req) const {
    req.method = Poco::Net::HTTPRequest::HTTP_GET;
    req.timeout_seconds = kHTTPClientTimeoutSeconds * 10;
    return request(req);
}

HTTPSResponse HTTPSClient::Delete(
    HTTPSRequest req) const {
    req.method = Poco::Net::HTTPRequest::HTTP_DELETE;
    return request(req);
}

HTTPSResponse HTTPSClient::Put(
    HTTPSRequest req) const {
    req.method = Poco::Net::HTTPRequest::HTTP_PUT;
    return request(req);
}

HTTPSResponse HTTPSClient::request(
    HTTPSRequest req) const {
    HTTPSResponse resp = makeHttpRequest(req);

    if (resp.err == error::kCannotConnectError && isRedirect(resp.status_code)) {
        // Reattempt request to the given location.
        Poco::URI uri(resp.body);

        req.host = uri.getScheme() + "://" + uri.getHost();
        req.relative_url = uri.getPathEtc();

        logger().debug("Redirect to URL=", resp.body, " host=", req.host, " relative_url=", req.relative_url);
        resp = makeHttpRequest(req);
    }
    return resp;
}

HTTPSResponse HTTPSClient::makeHttpRequest(
    HTTPSRequest req) const {

    HTTPSResponse resp;

    if (!urls::RequestsAllowed()) {
        resp.err = error::kCannotSyncInTestEnv;
        return resp;
    }

    if (urls::ImATeapot()) {
        resp.err = error::kUnsupportedAppError;
        return resp;
    }

    std::map<std::string, Poco::Timestamp>::const_iterator cit =
        banned_until_.find(req.host);
    if (cit != banned_until_.end()) {
        if (cit->second >= Poco::Timestamp()) {
            logger().warning(
                "Cannot connect, because we made too many requests");
            resp.err = error::kCannotConnectError;
            return resp;
        }
    }

    if (req.host.empty()) {
        resp.err = error::kMissingArgument;
        return resp;
    }
    if (req.method.empty()) {
        resp.err = error::kMissingArgument;
        return resp;
    }
    if (req.relative_url.empty()) {
        resp.err = error::kMissingArgument;
        return resp;
    }
    if (HTTPSClient::Config.CACertPath.empty()) {
        resp.err = error::kMissingArgument;
        return resp;
    }

    try {
        Poco::URI uri(req.host);

        Poco::SharedPtr<Poco::Net::InvalidCertificateHandler>
        acceptCertHandler =
            new Poco::Net::AcceptCertificateHandler(true);

        Poco::Net::Context::VerificationMode verification_mode =
            Poco::Net::Context::VERIFY_RELAXED;
        if (HTTPSClient::Config.IgnoreCert) {
            verification_mode = Poco::Net::Context::VERIFY_NONE;
        }
        Poco::Net::Context::Ptr context = new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE, "", "",
            HTTPSClient::Config.CACertPath,
            verification_mode, 9, true, "ALL");

        Poco::Net::SSLManager::instance().initializeClient(
            nullptr, acceptCertHandler, context);

        Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort(),
                                              context);

        session.setKeepAlive(false);
        session.setTimeout(
            Poco::Timespan(req.timeout_seconds * Poco::Timespan::SECONDS));

        logger().debug("Sending request to ", req.host, req.relative_url, " ..");

        std::string encoded_url("");
        Poco::URI::encode(req.relative_url, "", encoded_url);

        error err = Netconf::ConfigureProxy(req.host + encoded_url, &session);
        if (err != noError) {
            logger().log("Error while configuring proxy: ", err.String());
            resp.err = err;
            logger().error(resp.err);
            return resp;
        }

        Poco::Net::HTTPRequest poco_req(req.method,
                                        encoded_url,
                                        Poco::Net::HTTPMessage::HTTP_1_1);
        poco_req.setKeepAlive(false);

        // FIXME: should get content type as parameter instead
        if (req.payload.size()) {
            poco_req.setContentType(kContentTypeApplicationJSON);
        }
        poco_req.set("User-Agent", HTTPSClient::Config.UserAgent());

        Poco::Net::HTTPBasicCredentials cred(
            req.basic_auth_username, req.basic_auth_password);
        if (!req.basic_auth_username.empty()
                && !req.basic_auth_password.empty()) {
            cred.authenticate(poco_req);
        }

        if (!req.form) {
            std::istringstream requestStream(req.payload);

            Poco::DeflatingInputStream gzipRequest(
                requestStream,
                Poco::DeflatingStreamBuf::STREAM_GZIP);
            Poco::DeflatingStreamBuf *pBuff = gzipRequest.rdbuf();

            Poco::Int64 size =
                pBuff->pubseekoff(0, std::ios::end, std::ios::in);
            pBuff->pubseekpos(0, std::ios::in);

            if (req.method != Poco::Net::HTTPRequest::HTTP_GET) {
                poco_req.setContentLength(size);
                poco_req.set("Content-Encoding", "gzip");
                poco_req.setChunkedTransferEncoding(true);
            }

            session.sendRequest(poco_req) << pBuff << std::flush;
        } else {
            req.form->prepareSubmit(poco_req);
            std::ostream& send = session.sendRequest(poco_req);
            req.form->write(send);
        }

        // Request gzip unless downloading files
        poco_req.set("Accept-Encoding", "gzip");

        // Log out request contents
        std::stringstream request_string;
        poco_req.write(request_string);
        logger().debug(request_string.str());

        logger().debug("Request sent. Receiving response..");

        // Receive response
        Poco::Net::HTTPResponse response;
        std::istream& is = session.receiveResponse(response);

        resp.status_code = response.getStatus();

        {
            std::stringstream ss;
            ss << "Response status code " << response.getStatus()
               << ", content length " << response.getContentLength()
               << ", content type " << response.getContentType();
            if (response.has("Content-Encoding")) {
                ss << ", content encoding " << response.get("Content-Encoding");
            } else {
                ss << ", unknown content encoding";
            }
            logger().debug(ss.str());
        }

        // Log out X-Toggl-Request-Id, so failed requests can be traced
        if (response.has("X-Toggl-Request-Id")) {
            logger().debug("X-Toggl-Request-Id "
                           + response.get("X-Toggl-Request-Id"));
        }

        // Print out response headers
        Poco::Net::NameValueCollection::ConstIterator it = response.begin();
        while (it != response.end()) {
            logger().debug(it->first + ": " + it->second);
            ++it;
        }

        // When we get redirect, set the Location as response body
        if (isRedirect(resp.status_code) && response.has("Location")) {
            std::string decoded_url("");
            Poco::URI::decode(response.get("Location"), decoded_url);
            resp.body = decoded_url;

            // Inflate, if gzip was sent
        } else if (response.has("Content-Encoding") &&
                   "gzip" == response.get("Content-Encoding")) {
            Poco::InflatingInputStream inflater(is, Poco::InflatingStreamBuf::STREAM_GZIP);
            {
                std::stringstream ss;
                ss << inflater.rdbuf();
                resp.body = ss.str();
            }

            // Write the response to string
        } else {
            std::streamsize n = Poco::StreamCopier::copyToString(is, resp.body);
            logger().debug(n, " characters transferred with download");
        }

        logger().trace(resp.body);

        if (429 == resp.status_code) {
            Poco::Timestamp ts = Poco::Timestamp() + (60 * kOneSecondInMicros);
            banned_until_[req.host] = ts;

            logger().debug("Server indicated we're making too many requests to host ", req.host,
                           ". So we cannot make new requests until ", Formatter::Format8601(ts));
        }

        resp.err = Error::fromHttpStatus(resp.status_code);

        // Parse human-readable error message from response if Content Type JSON
        if (resp.err != noError &&
                response.getContentType().find(kContentTypeApplicationJSON) != std::string::npos) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(resp.body, root)) {
                resp.body = root["error_message"].asString();
            }
        }
    } catch(const Poco::Exception& exc) {
        resp.err = error::REMOVE_LATER_EXCEPTION_HANDLER;
        return resp;
    } catch(const std::exception& ex) {
        resp.err = error::REMOVE_LATER_EXCEPTION_HANDLER;
        return resp;
    } catch(const std::string & ex) {
        resp.err = error::REMOVE_LATER_EXCEPTION_HANDLER;
        return resp;
    }
    return resp;
}

ServerStatus TogglClient::TogglStatus;

Logger TogglClient::logger() const {
    return { "TogglClient" };
}

HTTPSResponse TogglClient::request(
    HTTPSRequest req) const {

    error err = TogglStatus.Status();
    if (err != noError) {
        logger().error("Will not connect, because of known bad Toggl status: ", err);
        HTTPSResponse resp;
        resp.err = err;
        return resp;
    }

    if (monitor_) {
        monitor_->DisplaySyncState(kSyncStateWork);
    }

    HTTPSResponse resp = HTTPSClient::request(req);

    if (monitor_) {
        monitor_->DisplaySyncState(kSyncStateIdle);
    }

    // We only update Toggl status from this
    // client, not websocket or regular http client,
    // as they are not critical.
    TogglStatus.UpdateStatus(resp.status_code);

    return resp;
}

}   // namespace toggl