import QtQuick 2.12
import QtQuick.Layouts 1.12

ColumnLayout {
    Layout.fillHeight: true
    Layout.fillWidth: true

    property QtObject timeEntry: null

    Text {
        Layout.fillWidth: true
        text: timeEntry.Description.length > 0 ? timeEntry.Description : "+ Add description"
        color: timeEntry.Description.length > 0 ? palette.text : disabledPalette.text
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        font.pixelSize: 12
        verticalAlignment: Text.AlignVCenter
    }
    RowLayout {
        Layout.fillWidth: true
        Rectangle {
            height: 3
            width: height
            radius: height / 2
            visible: timeEntry.ClientLabel.length > 0
            color: timeEntry.Color
            Layout.alignment: Qt.AlignVCenter
        }
        Text {
            visible: timeEntry.ClientLabel.length > 0
            text: timeEntry.ClientLabel
            color: timeEntry.Color.length > 0 ? timeEntry.Color : palette.text
            font.pixelSize: 12
        }
        Text {
            text: timeEntry.ProjectLabel.length > 0 ? timeEntry.ProjectLabel : "+ Add project"
            color: timeEntry.ProjectLabel.length > 0 ? palette.text : disabledPalette.text
            font.pixelSize: 12
        }
        Text {
            visible: timeEntry.TaskLabel.length > 0
            text: timeEntry.TaskLabel
            color: palette.text
            font.pixelSize: 12
        }
        Item {
            Layout.fillWidth: true
        }
    }
}

/*
Text {
    visible: running
    Layout.fillWidth: true
    verticalAlignment: Text.AlignVCenter
    text: runningTimeEntry && runningTimeEntry.Description.length > 0 ? " " + runningTimeEntry.Description : " (no description)"
    color: palette.text
    font.pixelSize: 12
}
RowLayout {
    visible: runningTimeEntry
    Rectangle {
        antialiasing: true
        visible: runningTimeEntry && runningTimeEntry.ProjectLabel.length > 0
        height: 8
        width: height
        radius: height / 2
        color: runningTimeEntry && runningTimeEntry.Color.length > 0 ? runningTimeEntry.Color : palette.text
    }
    Text {
        visible: runningTimeEntry && runningTimeEntry.ProjectLabel.length > 0
        text: runningTimeEntry ? runningTimeEntry.ProjectLabel : ""
        color: runningTimeEntry && runningTimeEntry.Color.length > 0 ? runningTimeEntry.Color : palette.text
        font.pixelSize: 11
    }
    Text {
        visible: runningTimeEntry && runningTimeEntry.TaskLabel.length > 0
        text: runningTimeEntry && runningTimeEntry.TaskLabel.length ? "- " + runningTimeEntry.TaskLabel : ""
        color: palette.text
        font.pixelSize: 11
    }
    Text {
        //visible: runningTimeEntry && runningTimeEntry.ClientLabel.length > 0
        text: runningTimeEntry ? "• " + runningTimeEntry.ClientLabel : ""
        color: palette.text
        font.pixelSize: 11
    }
}
*/