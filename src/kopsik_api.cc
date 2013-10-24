// Copyright 2013 Tanel Lebedev

#include <cstring>

#include "./kopsik_api.h"
#include "./database.h"
#include "./toggl_api_client.h"
#include "./https_client.h"
#include "./websocket_client.h"
#include "./version.h"
#include "./timeline_database.h"
#include "./timeline_uploader.h"
#include "./window_change_recorder.h"

#include "Poco/Bugcheck.h"
#include "Poco/Path.h"
#include "Poco/Logger.h"
#include "Poco/SimpleFileChannel.h"
#include "Poco/FormattingChannel.h"
#include "Poco/PatternFormatter.h"
#include "Poco/ScopedLock.h"
#include "Poco/Mutex.h"
#include "Poco/Thread.h"
#include "Poco/Runnable.h"
#include "Poco/TaskManager.h"
#include "Poco/Task.h"

#define KOPSIK_API_FAILURE 1

// Private helpers

KopsikModelChange *model_change_init() {
  KopsikModelChange *change = new KopsikModelChange();
  change->ModelType = 0;
  change->ChangeType = 0;
  change->ModelID = 0;
  change->GUID = 0;
  return change;
}

void model_change_clear(KopsikModelChange *change) {
  poco_assert(change);
  if (change->ModelType) {
    free(change->ModelType);
    change->ModelType = 0;
  }
  if (change->ChangeType) {
    free(change->ChangeType);
    change->ChangeType = 0;
  }
  if (change->GUID) {
    free(change->GUID);
    change->GUID = 0;
  }
  delete change;
  change = 0;
}

void model_change_to_change_item(
    kopsik::ModelChange &in,
    KopsikModelChange &out) {

  poco_assert(in.ModelType() == "time_entry" ||
    in.ModelType() == "workspace" ||
    in.ModelType() == "client" ||
    in.ModelType() == "project" ||
    in.ModelType() == "user" ||
    in.ModelType() == "task" ||
    in.ModelType() == "tag");

  poco_assert(in.ChangeType() == "delete" ||
    in.ChangeType() == "insert" ||
    in.ChangeType() == "update");

  poco_assert(!in.GUID().empty() || in.ModelID() > 0);

  poco_assert(!out.ModelType);
  out.ModelType = strdup(in.ModelType().c_str());

  out.ModelID = (unsigned int)in.ModelID();

  poco_assert(!out.ChangeType);
  out.ChangeType = strdup(in.ChangeType().c_str());

  poco_assert(!out.GUID);
  out.GUID = strdup(in.GUID().c_str());
}

typedef struct {
  kopsik::Database *db;
  kopsik::User *user;
  kopsik::HTTPSClient *https_client;
  kopsik::WebSocketClient *ws_client;
  Poco::Mutex *mutex;
  Poco::TaskManager *tm;
  KopsikViewItemChangeCallback change_callback;
  TimelineDatabase *timeline_database;
  TimelineUploader *timeline_uploader;
  WindowChangeRecorder *window_change_recorder;
  std::string app_name;
  std::string app_version;
} Context;

void kopsik_set_change_callback(
    void *context,
    KopsikViewItemChangeCallback callback) {
  poco_assert(callback);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_set_change_callback");

  Context *ctx = reinterpret_cast<Context *>(context);
  Poco::Mutex::ScopedLock lock(*ctx->mutex);
  ctx->change_callback = callback;
}

kopsik_api_result save(Context *ctx,
    char *errmsg, unsigned int errlen) {
  poco_assert(ctx);
  poco_assert(errmsg);
  poco_assert(errlen);

  std::vector<kopsik::ModelChange> changes;
  kopsik::error err = ctx->db->SaveUser(ctx->user, true, &changes);

  if (err != kopsik::noError) {
    if (ctx->change_callback) {
      ctx->change_callback(KOPSIK_API_FAILURE, err.c_str(), 0);
    }
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  if (!ctx->change_callback) {
    return KOPSIK_API_SUCCESS;
  }

  for (std::vector<kopsik::ModelChange>::const_iterator it = changes.begin();
      it != changes.end();
      it++) {
    kopsik::ModelChange mc = *it;
    KopsikModelChange *change = model_change_init();
    model_change_to_change_item(mc, *change);
    ctx->change_callback(KOPSIK_API_SUCCESS, 0, change);
    model_change_clear(change);
  }

  return KOPSIK_API_SUCCESS;
}

void time_entry_to_view_item(
    kopsik::TimeEntry *te,
    kopsik::User *user,
    KopsikTimeEntryViewItem *view_item) {
  poco_assert(te);
  poco_assert(user);
  poco_assert(view_item);

  view_item->DurationInSeconds = static_cast<int>(te->DurationInSeconds());

  poco_assert(!view_item->Description);
  view_item->Description = strdup(te->Description().c_str());

  poco_assert(!view_item->GUID);
  view_item->GUID = strdup(te->GUID().c_str());

  if (te->PID()) {
    kopsik::Project *p = user->GetProjectByID(te->PID());
    if (p) {
      poco_assert(!view_item->Project);

      view_item->Project = strdup(p->UppercaseName().c_str());

      poco_assert(!view_item->Color);
      view_item->Color = strdup(p->ColorCode().c_str());
    }
  }

  poco_assert(!view_item->Duration);
  view_item->Duration = strdup(te->DurationString().c_str());

  view_item->Started = static_cast<unsigned int>(te->Start());
  view_item->Ended = static_cast<unsigned int>(te->Stop());
  if (te->Billable()) {
    view_item->Billable = 1;
  } else {
    view_item->Billable = 0;
  }

  poco_assert(!view_item->Tags);
  if (!te->Tags().empty()) {
    view_item->Tags = strdup(te->Tags().c_str());
  }

  poco_assert(!view_item->UpdatedAt);
  std::string updated_at = te->UpdatedAtString();
  if (!updated_at.empty()) {
    view_item->UpdatedAt = strdup(updated_at.c_str());
  }
}

// Context API.

void *kopsik_context_init(const char *app_name, const char *app_version) {
  poco_assert(app_name);
  poco_assert(app_version);

  Context *ctx = new Context();

  ctx->db = 0;
  ctx->timeline_database = new TimelineDatabase();
  ctx->timeline_uploader = new TimelineUploader();
  ctx->window_change_recorder = new WindowChangeRecorder();
  ctx->user = 0;

  ctx->app_name = std::string(app_name);
  ctx->app_version = std::string(app_version);

  ctx->https_client = new kopsik::HTTPSClient("https://www.toggl.com",
    ctx->app_name, ctx->app_version);
  ctx->ws_client = new kopsik::WebSocketClient("https://stream.toggl.com",
    ctx->app_name, ctx->app_version);

  ctx->mutex = new Poco::Mutex();
  ctx->tm = new Poco::TaskManager();

  ctx->change_callback = 0;

  return ctx;
}

void kopsik_context_clear(void *context) {
  poco_assert(context);

  Context *ctx = reinterpret_cast<Context *>(context);

  if (ctx->tm) {
    ctx->tm->joinAll();
    delete ctx->tm;
    ctx->tm = 0;
  }
  if (ctx->db) {
    delete ctx->db;
    ctx->db = 0;
  }
  if (ctx->user) {
    delete ctx->user;
    ctx->user = 0;
  }
  if (ctx->https_client) {
    delete ctx->https_client;
    ctx->https_client = 0;
  }
  if (ctx->ws_client) {
    delete ctx->ws_client;
    ctx->ws_client = 0;
  }
  if (ctx->mutex) {
    delete ctx->mutex;
    ctx->mutex = 0;
  }

  delete ctx->timeline_database;
  ctx->timeline_database = 0;

  delete ctx->timeline_uploader;
  ctx->timeline_uploader = 0;

  delete ctx->window_change_recorder;
  ctx->window_change_recorder = 0;

  delete ctx;
  ctx = 0;
}

// Configuration API.

KopsikProxySettings *kopsik_proxy_settings_init() {
  KopsikProxySettings *settings = new KopsikProxySettings();
  settings->UseProxy = 0;
  settings->Host = 0;
  settings->Port = 0;
  settings->Username = 0;
  settings->Password = 0;
  return settings;
}

void kopsik_proxy_settings_clear(KopsikProxySettings *settings) {
  if (settings->Host) {
    free(settings->Host);
    settings->Host = 0;
  }
  if (settings->Username) {
    free(settings->Username);
    settings->Username = 0;
  }
  if (settings->Password) {
    free(settings->Password);
    settings->Password = 0;
  }
  delete settings;
  settings = 0;
}

kopsik_api_result kopsik_get_proxy(void *context,
    char *errmsg,
    unsigned int errlen,
    KopsikProxySettings *settings) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(settings);

  Context *ctx = reinterpret_cast<Context *>(context);

  int use_proxy(0);
  std::string host("");
  unsigned int port(0);
  std::string username("");
  std::string password("");

  kopsik::error err = ctx->db->LoadProxySettings(
    &use_proxy, &host, &port, &username, &password);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  settings->UseProxy = use_proxy;
  poco_assert(!settings->Host);
  settings->Host = strdup(host.c_str());
  settings->Port = port;
  poco_assert(!settings->Username);
  settings->Username = strdup(username.c_str());
  poco_assert(!settings->Password);
  settings->Password = strdup(password.c_str());

  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_set_proxy(
    void *context,
    char *errmsg,
    unsigned int errlen,
    const int use_proxy,
    const char *host,
    const unsigned int port,
    const char *username,
    const char *password) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(host);
  poco_assert(username);
  poco_assert(password);

  std::stringstream ss;
  ss  << "kopsik_set_proxy use_proxy=" << use_proxy
      << ", host=" << host
      << ", port=" << port
      << ", username=" << username
      << ", password=" << password;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::error err =
    ctx->db->SaveProxySettings(use_proxy, host, port, username, password);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  return KOPSIK_API_SUCCESS;
}

void kopsik_test_set_https_client(void *context, void *client) {
  poco_assert(context);
  poco_assert(client);
  Context *ctx = reinterpret_cast<Context *>(context);
  if (ctx->https_client) {
    delete ctx->https_client;
    ctx->https_client = 0;
  }
  ctx->https_client = reinterpret_cast<kopsik::HTTPSClient *>(client);
}

void kopsik_set_db_path(void *context, const char *path) {
  poco_assert(context);
  poco_assert(path);

  std::stringstream ss;
  ss  << "kopsik_set_db_path path=" << path;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  if (ctx->db) {
    delete ctx->db;
    ctx->db = 0;
  }
  ctx->db = new kopsik::Database(path);
}

void kopsik_set_log_path(void *context, const char *path) {
  poco_assert(context);
  poco_assert(path);

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  Poco::AutoPtr<Poco::SimpleFileChannel> simpleFileChannel(
    new Poco::SimpleFileChannel);
  simpleFileChannel->setProperty("path", path);
  simpleFileChannel->setProperty("rotation", "1 M");

  Poco::AutoPtr<Poco::FormattingChannel> formattingChannel(
      new Poco::FormattingChannel(
        new Poco::PatternFormatter("%Y-%m-%d %H:%M:%S.%c [%P]:%s:%q:%t")));
  formattingChannel->setChannel(simpleFileChannel);

  Poco::Logger &rootLogger = Poco::Logger::get("");
  rootLogger.setChannel(formattingChannel);
  rootLogger.setLevel(Poco::Message::PRIO_DEBUG);
}

void kopsik_set_log_level(void *context,
                          const char *level) {
  poco_assert(context);
  poco_assert(level);

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  Poco::Logger &rootLogger = Poco::Logger::get("");
  rootLogger.setLevel(level);
}

void kopsik_set_api_url(void *context, const char *api_url) {
  poco_assert(context);
  poco_assert(api_url);

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  ctx->https_client->SetApiURL(api_url);
}

void kopsik_set_websocket_url(void *context, const char *websocket_url) {
  poco_assert(context);
  poco_assert(websocket_url);

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  ctx->ws_client->SetWebsocketURL(websocket_url);
}

// User API.

KopsikUser *kopsik_user_init() {
  KopsikUser *user = new KopsikUser();
  user->ID = 0;
  user->Fullname = 0;
  return user;
}

void kopsik_user_clear(KopsikUser *user) {
  poco_assert(user);
  user->ID = 0;
  if (user->Fullname) {
    free(user->Fullname);
    user->Fullname = 0;
  }
  delete user;
  user = 0;
}

kopsik_api_result kopsik_current_user(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikUser *out_user) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(out_user);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_current_user");

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  if (!ctx->user) {
    kopsik::User *user = new kopsik::User(ctx->app_name, ctx->app_version);
    kopsik::error err = ctx->db->LoadCurrentUser(user, true);
    if (err != kopsik::noError) {
      delete user;
      strncpy(errmsg, err.c_str(), errlen);
      return KOPSIK_API_FAILURE;
    }
    ctx->user = user;
  }
  poco_assert(ctx->user);
  if (out_user->Fullname) {
    free(out_user->Fullname);
    out_user->Fullname = 0;
  }
  out_user->Fullname = strdup(ctx->user->Fullname().c_str());
  out_user->ID = (unsigned int)ctx->user->ID();
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_set_api_token(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *api_token) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(api_token);

  std::stringstream ss;
  ss << "kopsik_set_api_token api_token=" << api_token;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::error err = ctx->db->SetCurrentAPIToken(api_token);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_get_api_token(
    void *context,
    char *errmsg, unsigned int errlen,
    char *str, unsigned int max_strlen) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(str);
  poco_assert(max_strlen);

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  std::string token("");
  kopsik::error err = ctx->db->CurrentAPIToken(&token);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }
  strncpy(str, token.c_str(), max_strlen);
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_login(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *in_email, const char *in_password) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(in_email);
  poco_assert(in_password);

  std::stringstream ss;
  ss  << "kopik_login email=" << in_email;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string email(in_email);
  std::string password(in_password);
  if (email.empty()) {
    strncpy(errmsg, "Empty email", errlen);
    return KOPSIK_API_FAILURE;
  }
  if (password.empty()) {
    strncpy(errmsg, "Empty password", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  if (ctx->user) {
    delete ctx->user;
    ctx->user = 0;
  }

  kopsik::User *user = new kopsik::User(ctx->app_name, ctx->app_version);
  kopsik::error err = user->Login(ctx->https_client, email, password);
  if (err != kopsik::noError) {
    delete user;
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  std::vector<kopsik::ModelChange> changes;
  err = ctx->db->SaveUser(user, true, &changes);
  if (err != kopsik::noError) {
    delete user;
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  err = ctx->db->SetCurrentAPIToken(user->APIToken());
  if (err != kopsik::noError) {
    delete user;
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  ctx->user = user;
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_logout(
    void *context,
    char *errmsg, unsigned int errlen) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_logout");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    return KOPSIK_API_SUCCESS;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::error err = ctx->db->ClearCurrentAPIToken();
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }

  if (ctx->user) {
    delete ctx->user;
    ctx->user = 0;
  }

  return KOPSIK_API_SUCCESS;
}

// Sync

kopsik_api_result kopsik_sync(
    void *context,
    char *errmsg, unsigned int errlen,
    int full_sync) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_sync");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::error err = ctx->user->Sync(ctx->https_client, full_sync);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }
  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_push(
    void *context,
    char *errmsg, unsigned int errlen) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_push");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::error err = ctx->user->Push(ctx->https_client);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }
  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_pushable_models(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikPushableModelStats *stats) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(stats);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_pushable_models");

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }
  std::vector<kopsik::TimeEntry *> pushable;
  ctx->user->CollectPushableObjects(&pushable);
  stats->TimeEntries = 0;
  for (std::vector<kopsik::TimeEntry *>::const_iterator it = pushable.begin();
    it != pushable.end();
    it++) {
    stats->TimeEntries++;
  }
  return KOPSIK_API_SUCCESS;
}

class SyncTask : public Poco::Task {
  public:
    SyncTask(Context *ctx,
      int full_sync,
      KopsikResultCallback callback) : Task("sync"),
      ctx_(ctx),
      full_sync_(full_sync),
      callback_(callback) {}
    void runTask() {
      char err[KOPSIK_ERR_LEN];
      kopsik_api_result res = kopsik_sync(
        ctx_, err, KOPSIK_ERR_LEN, full_sync_);
      callback_(res, err);
    }
  private:
    Context *ctx_;
    int full_sync_;
    KopsikResultCallback callback_;
};

void kopsik_sync_async(
    void *context,
    int full_sync,
    KopsikResultCallback callback) {
  poco_assert(context);
  poco_assert(callback);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_sync_async");

  Context *ctx = reinterpret_cast<Context *>(context);

  ctx->tm->start(new SyncTask(ctx, full_sync, callback));
}

class PushTask : public Poco::Task {
  public:
    PushTask(Context *ctx,
      KopsikResultCallback callback) : Task("push"),
      ctx_(ctx), callback_(callback) {}
    void runTask() {
      char err[KOPSIK_ERR_LEN];
      kopsik_api_result res = kopsik_push(ctx_, err, KOPSIK_ERR_LEN);
      callback_(res, err);
    }
  private:
    Context *ctx_;
    KopsikResultCallback callback_;
};

void kopsik_push_async(
    void *context,
    KopsikResultCallback callback) {
  poco_assert(context);
  poco_assert(callback);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_push_async");

  Context *ctx = reinterpret_cast<Context *>(context);

  ctx->tm->start(new PushTask(ctx, callback));
}

// Project list

KopsikProjectSelectItemList *
    kopsik_project_select_item_list_init() {
  KopsikProjectSelectItemList *result = new KopsikProjectSelectItemList();
  result->Length = 0;
  result->ViewItems = 0;
  return result;
}

void project_select_item_clear(KopsikProjectSelectItem *item) {
  if (item->Name) {
    free(item->Name);
    item->Name = 0;
  }
  delete item;
}

void kopsik_project_select_item_list_clear(
    KopsikProjectSelectItemList *list) {
  poco_assert(list);
  for (unsigned int i = 0; i < list->Length; i++) {
    KopsikProjectSelectItem *item = list->ViewItems[i];
    poco_assert(item);
    project_select_item_clear(item);
    list->ViewItems[i] = 0;
  }
  if (list->ViewItems) {
    free(list->ViewItems);
  }
  delete list;
  list = 0;
}

KopsikProjectSelectItem *project_select_item_init() {
  KopsikProjectSelectItem *item = new KopsikProjectSelectItem();
  item->Name = 0;
  return item;;
}

kopsik_api_result kopsik_project_select_items(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikProjectSelectItemList *list) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(list);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_project_select_items");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  std::vector<kopsik::Project *>active;
  for (unsigned int i = 0; i < ctx->user->related.Projects.size(); i++) {
    kopsik::Project *p = ctx->user->related.Projects[i];
    if (p->Active()) {
      active.push_back(p);
    }
  }

  list->Length = 0;

  KopsikProjectSelectItem *tmp = project_select_item_init();
  void *m = malloc(active.size() * sizeof(tmp));
  project_select_item_clear(tmp);
  poco_assert(m);
  list->ViewItems =
    reinterpret_cast<KopsikProjectSelectItem **>(m);
  for (unsigned int i = 0; i < active.size(); i++) {
    kopsik::Project *p = active[i];
    KopsikProjectSelectItem *view_item = project_select_item_init();
    view_item->Name = strdup(p->Name().c_str());
    list->ViewItems[i] = view_item;
    list->Length++;
  }
  return KOPSIK_API_SUCCESS;
}

// Time entries view API

KopsikTimeEntryViewItem *kopsik_time_entry_view_item_init() {
  KopsikTimeEntryViewItem *item = new KopsikTimeEntryViewItem();
  item->DurationInSeconds = 0;
  item->Description = 0;
  item->Project = 0;
  item->Duration = 0;
  item->Color = 0;
  item->GUID = 0;
  item->Billable = 0;
  item->Tags = 0;
  item->Started = 0;
  item->Ended = 0;
  item->UpdatedAt = 0;
  return item;
}

void kopsik_time_entry_view_item_clear(KopsikTimeEntryViewItem *item) {
  poco_assert(item);
  if (item->Description) {
    free(item->Description);
    item->Description = 0;
  }
  if (item->Project) {
    free(item->Project);
    item->Project = 0;
  }
  if (item->Duration) {
    free(item->Duration);
    item->Duration = 0;
  }
  if (item->Color) {
    free(item->Color);
    item->Color = 0;
  }
  if (item->GUID) {
    free(item->GUID);
    item->GUID = 0;
  }
  if (item->Tags) {
    free(item->Tags);
    item->Tags = 0;
  }
  if (item->UpdatedAt) {
    free(item->UpdatedAt);
    item->UpdatedAt = 0;
  }
  delete item;
  item = 0;
}

void kopsik_format_duration_in_seconds_hhmmss(
    int duration_in_seconds, char *out_str, unsigned int max_strlen) {
  poco_assert(out_str);
  poco_assert(max_strlen);
  std::string formatted =
    kopsik::Formatter::FormatDurationInSecondsHHMMSS(duration_in_seconds);
  strncpy(out_str, formatted.c_str(), max_strlen);
}

void kopsik_format_duration_in_seconds_hhmm(
    int duration_in_seconds, char *out_str, unsigned int max_strlen) {
  poco_assert(out_str);
  poco_assert(max_strlen);
  std::string formatted =
    kopsik::Formatter::FormatDurationInSecondsHHMM(duration_in_seconds);
  strncpy(out_str, formatted.c_str(), max_strlen);
}

kopsik_api_result kopsik_start(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *in_description,
    KopsikTimeEntryViewItem *out_view_item) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(in_description);
  poco_assert(out_view_item);

  std::stringstream ss;
  ss << "kopsik_start description=" << in_description;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  std::string description(in_description);
  if (description.empty()) {
    strncpy(errmsg, "Missing description", errlen);
    return KOPSIK_API_FAILURE;
  }
  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }
  kopsik::TimeEntry *te = ctx->user->Start(description);
  kopsik_api_result res = save(ctx, errmsg, errlen);
  if (KOPSIK_API_SUCCESS != res) {
    return res;
  }
  time_entry_to_view_item(te, ctx->user, out_view_item);
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_time_entry_view_item_by_guid(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    KopsikTimeEntryViewItem *view_item,
    int *was_found) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(view_item);
  poco_assert(was_found);

  std::stringstream ss;
  ss << "kopsik_time_entry_view_item_by_guid guid=" << guid;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  Context *ctx = reinterpret_cast<Context *>(context);
  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }
  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }
  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  if (te) {
    *was_found = 1;
    time_entry_to_view_item(te, ctx->user, view_item);
  } else {
    *was_found = 0;
  }
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_continue(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    KopsikTimeEntryViewItem *view_item) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(view_item);

  std::stringstream ss;
  ss << "kopsik_continue guid=" << guid;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->Continue(GUID);
  poco_assert(te);
  kopsik_api_result res = save(ctx, errmsg, errlen);
  if (KOPSIK_API_SUCCESS != res) {
    return res;
  }
  time_entry_to_view_item(te, ctx->user, view_item);
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_continue_latest(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikTimeEntryViewItem *view_item,
    int *was_found) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(view_item);
  poco_assert(was_found);

  std::stringstream ss;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_continue_latest");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  ctx->user->SortTimeEntriesByStart();
  kopsik::TimeEntry *latest = ctx->user->Latest();
  if (!latest) {
    *was_found = 0;
    return KOPSIK_API_SUCCESS;
  }

  kopsik::TimeEntry *te = ctx->user->Continue(latest->GUID());
  poco_assert(te);
  kopsik_api_result res = save(ctx, errmsg, errlen);
  if (KOPSIK_API_SUCCESS != res) {
    return res;
  }
  *was_found = 1;
  time_entry_to_view_item(te, ctx->user, view_item);
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_delete_time_entry(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);

  std::stringstream ss;
  ss << "kopsik_delete_time_entry guid=" << guid;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  ctx->user->MarkTimeEntryAsDeleted(GUID);
  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_duration(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    const char *value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(value);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_duration guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);
  te->SetDurationString(std::string(value));
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_project(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    const char *value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_project guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);

  std::string project_name(value);
  unsigned int project_id = 0;
  if (!project_name.empty()) {
    kopsik::Project *p = ctx->user->GetProjectByName(std::string(value));
    if (p) {
      project_id = (unsigned int)p->ID();
    }
  }
  te->SetPID(project_id);
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_start_iso_8601(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    const char *value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(value);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_start_iso_8601 guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);
  te->SetStartString(std::string(value));
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_end_iso_8601(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    const char *value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(value);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_end_iso_8601 guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);
  te->SetStopString(std::string(value));
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_tags(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    const char *value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(value);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_tags guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);
  te->SetTags(std::string(value));
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_billable(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    int value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_billable guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);
  if (value) {
    te->SetBillable(true);
  } else {
    te->SetBillable(false);
  }
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_set_time_entry_description(
    void *context,
    char *errmsg, unsigned int errlen,
    const char *guid,
    const char *value) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(guid);
  poco_assert(value);

  std::stringstream ss;
  ss  << "kopsik_set_time_entry_description guid=" << guid
      << ", value=" << value;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  std::string GUID(guid);
  if (GUID.empty()) {
    strncpy(errmsg, "Missing GUID", errlen);
    return KOPSIK_API_FAILURE;
  }

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  kopsik::TimeEntry *te = ctx->user->GetTimeEntryByGUID(GUID);
  poco_assert(te);
  te->SetDescription(std::string(value));
  if (te->Dirty()) {
    te->SetUIModifiedAt(time(0));
  }

  return save(ctx, errmsg, errlen);
}

kopsik_api_result kopsik_stop(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikTimeEntryViewItem *out_view_item,
    int *was_found) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(out_view_item);
  poco_assert(was_found);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_stop");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  std::vector<kopsik::TimeEntry *> stopped = ctx->user->Stop();
  if (stopped.empty()) {
    *was_found = 0;
    return KOPSIK_API_SUCCESS;
  }
  *was_found = 1;
  kopsik_api_result res = save(ctx, errmsg, errlen);
  if (res != KOPSIK_API_SUCCESS) {
    return res;
  }
  kopsik::TimeEntry *te = stopped[0];
  time_entry_to_view_item(te, ctx->user, out_view_item);
  return KOPSIK_API_SUCCESS;
}

kopsik_api_result kopsik_running_time_entry_view_item(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikTimeEntryViewItem *out_item, int *out_is_tracking) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(out_item);
  poco_assert(out_is_tracking);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_running_time_entry_view_item");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  *out_is_tracking = 0;

  kopsik::TimeEntry *te = ctx->user->RunningTimeEntry();
  if (te) {
    *out_is_tracking = true;
    time_entry_to_view_item(te, ctx->user, out_item);
  }
  return KOPSIK_API_SUCCESS;
}

KopsikTimeEntryViewItemList *kopsik_time_entry_view_item_list_init() {
  KopsikTimeEntryViewItemList *result = new KopsikTimeEntryViewItemList();
  result->Length = 0;
  result->ViewItems = 0;
  return result;
}

void kopsik_time_entry_view_item_list_clear(
    KopsikTimeEntryViewItemList *in_list) {
  poco_assert(in_list);
  for (unsigned int i = 0; i < in_list->Length; i++) {
    kopsik_time_entry_view_item_clear(in_list->ViewItems[i]);
    in_list->ViewItems[i] = 0;
  }
  if (in_list->ViewItems) {
    free(in_list->ViewItems);
  }
  delete in_list;
  in_list = 0;
}

kopsik_api_result kopsik_time_entry_view_items(
    void *context,
    char *errmsg, unsigned int errlen,
    KopsikTimeEntryViewItemList *out_list) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);
  poco_assert(out_list);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_time_entry_view_items");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  ctx->user->SortTimeEntriesByStart();

  std::vector<kopsik::TimeEntry *>visible;
  for (std::vector<kopsik::TimeEntry *>::const_iterator it =
      ctx->user->related.TimeEntries.begin();
      it != ctx->user->related.TimeEntries.end(); it++) {
    kopsik::TimeEntry *te = *it;
    poco_assert(!te->GUID().empty());
    if (te->DurationInSeconds() < 0) {
      continue;
    }
    if (te->DeletedAt() > 0) {
      continue;
    }
    visible.push_back(te);
  }

  if (visible.empty()) {
    return KOPSIK_API_SUCCESS;
  }

  out_list->Length = 0;

  KopsikTimeEntryViewItem *tmp = kopsik_time_entry_view_item_init();
  void *m = malloc(visible.size() * sizeof(tmp));
  kopsik_time_entry_view_item_clear(tmp);
  poco_assert(m);
  out_list->ViewItems =
    reinterpret_cast<KopsikTimeEntryViewItem **>(m);
  for (unsigned int i = 0; i < visible.size(); i++) {
    kopsik::TimeEntry *te = visible[i];
    KopsikTimeEntryViewItem *view_item = kopsik_time_entry_view_item_init();
    time_entry_to_view_item(te, ctx->user, view_item);
    out_list->ViewItems[i] = view_item;
    out_list->Length++;
  }
  return KOPSIK_API_SUCCESS;
}

// Websocket client

void on_websocket_message(
    void *context,
    std::string json) {
  poco_assert(context);
  poco_assert(!json.empty());

  std::stringstream ss;
  ss << "on_websocket_message json=" << json;
  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug(ss.str());

  Context *ctx = reinterpret_cast<Context *>(context);

  Poco::Mutex::ScopedLock lock(*ctx->mutex);

  ctx->user->LoadUpdateFromJSONString(json);

  char err[KOPSIK_ERR_LEN];
  save(ctx, err, KOPSIK_ERR_LEN);
}

kopsik_api_result kopsik_websocket_start(
    void *context,
    char *errmsg, unsigned int errlen) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_websocket_start");

  Context *ctx = reinterpret_cast<Context *>(context);

  if (!ctx->user) {
    strncpy(errmsg, "Please login first", errlen);
    return KOPSIK_API_FAILURE;
  }

  kopsik::error err = ctx->ws_client->Start(
    ctx,
    ctx->user->APIToken(),
    on_websocket_message);
  if (err != kopsik::noError) {
    strncpy(errmsg, err.c_str(), errlen);
    return KOPSIK_API_FAILURE;
  }
  return KOPSIK_API_SUCCESS;
}

class WebSocketStartTask : public Poco::Task {
  public:
    WebSocketStartTask(Context *ctx, KopsikResultCallback callback) :
      Task("start_websocket"),
      ctx_(ctx),
      callback_(callback) {}
    void runTask() {
      char err[KOPSIK_ERR_LEN];
      kopsik_api_result res = kopsik_websocket_start(
        ctx_, err, KOPSIK_ERR_LEN);
      callback_(res, err);
    }
  private:
    Context *ctx_;
    KopsikResultCallback callback_;
};

void kopsik_websocket_start_async(
    void *context,
    KopsikResultCallback callback) {
  poco_assert(context);
  poco_assert(callback);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_websocket_start_async");

  Context *ctx = reinterpret_cast<Context *>(context);

  ctx->tm->start(new WebSocketStartTask(ctx, callback));
}

kopsik_api_result kopsik_websocket_stop(
    void *context,
    char *errmsg, unsigned int errlen) {
  poco_assert(context);
  poco_assert(errmsg);
  poco_assert(errlen);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_websocket_stop");

  Context *ctx = reinterpret_cast<Context *>(context);

  ctx->ws_client->Stop();
  return KOPSIK_API_SUCCESS;
}

class WebSocketStopTask : public Poco::Task {
  public:
    WebSocketStopTask(Context *ctx, KopsikResultCallback callback) :
      Task("stop_websocket"),
      ctx_(ctx),
      callback_(callback) {}
    void runTask() {
      char err[KOPSIK_ERR_LEN];
      kopsik_api_result res = kopsik_websocket_stop(
        ctx_, err, KOPSIK_ERR_LEN);
      callback_(res, err);
    }
  private:
    Context *ctx_;
    KopsikResultCallback callback_;
};

void kopsik_websocket_stop_async(
    void *context,
    KopsikResultCallback callback) {
  poco_assert(context);
  poco_assert(callback);

  Poco::Logger &logger = Poco::Logger::get("kopsik_api");
  logger.debug("kopsik_websocket_stop_async");

  Context *ctx = reinterpret_cast<Context *>(context);

  ctx->tm->start(new WebSocketStopTask(ctx, callback));
}
