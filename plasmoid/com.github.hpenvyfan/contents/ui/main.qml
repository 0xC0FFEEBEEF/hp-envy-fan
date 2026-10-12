import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasma5support as P5Support
import org.kde.plasma.plasmoid

PlasmoidItem {
    id: root

    readonly property var cfg: Plasmoid.configuration
    readonly property string helper: decodeURIComponent(Qt.resolvedUrl("../code/envy-fanctl").toString().replace(/^file:\/\//, ""))
    readonly property string statusCmd: "python3 " + q(helper) + " status --limit " + cfg.limit + " --near " + cfg.nearMargin

    property var st: ({})
    property bool started: false
    property double lastApply: 0
    property string lastError: ""
    property int throttleStreak: 0
    property int nearStreak: 0
    property double lastThrottleNote: 0
    property double lastNearNote: 0
    property string lastEmergency: "none"

    readonly property bool haveTemp: st.temp !== undefined && st.temp !== null
    readonly property string thermalState: st.state || "unknown"
    readonly property color stateColor: {
        switch (thermalState) {
        case "ok": return Kirigami.Theme.positiveTextColor;
        case "near": return Kirigami.Theme.neutralTextColor;
        case "throttling": return Kirigami.Theme.negativeTextColor;
        default: return Kirigami.Theme.textColor;
        }
    }
    readonly property string stateText: {
        switch (thermalState) {
        case "ok": return "Cool";
        case "warm": return "Warm";
        case "near": return "Near throttling";
        case "throttling": return "Throttling";
        default: return "No reading";
        }
    }
    readonly property string modeText: !st.driver ? "driver not loaded"
        : !st.curve_enabled ? "HP auto"
        : cfg.mode === "fixed" ? "fixed " + cfg.fixedDuty + "%" : "curve"

    Plasmoid.icon: thermalState === "throttling" || thermalState === "near" ? "temperature-warm" : "temperature-normal"
    Plasmoid.status: thermalState === "throttling" ? PlasmaCore.Types.NeedsAttentionStatus : PlasmaCore.Types.ActiveStatus

    toolTipMainText: haveTemp ? "CPU " + Math.round(st.temp) + "°C · " + stateText : "Envy Fan"
    toolTipSubText: {
        if (!st.driver)
            return "hp_envy_fan driver is not loaded";
        let s = Math.round(st.headroom) + "°C below the " + st.limit + "°C throttle point";
        if (st.fans && st.fans.length === 2)
            s += "\nFans " + st.fans[0].duty + "% / " + st.fans[1].duty + "% · " + modeText;
        return s;
    }

    function q(s) {
        return "'" + String(s).replace(/'/g, "'\\''") + "'";
    }

    function curveSpec() {
        return cfg.startTemp + ":" + cfg.startDuty + " " + cfg.fullTemp + ":" + cfg.fullDuty;
    }

    function applySettings() {
        let cmd = "python3 " + q(helper) + " apply ";
        if (cfg.mode === "auto")
            cmd += "auto";
        else if (cfg.mode === "fixed")
            cmd += "fixed " + cfg.fixedDuty;
        else
            cmd += "curve " + q(curveSpec());
        cmd += " --smooth " + cfg.smooth;
        exec.connectSource(cmd + " # " + Date.now());
    }

    function setMode(mode) {
        cfg.hpMode = mode;
        exec.connectSource("python3 " + q(helper) + " mode " + mode + " # " + Date.now());
    }

    // What the driver should look like with the saved settings.
    function driverMatches(s) {
        if (s.smooth !== undefined && s.smooth !== null && s.smooth !== cfg.smooth)
            return false;
        if (cfg.mode === "auto")
            return !s.curve_enabled;
        const want = cfg.mode === "fixed" ? "0:" + cfg.fixedDuty : curveSpec();
        return s.curve_enabled && s.curve === want;
    }

    function notify(title, body, icon, urgency) {
        exec.connectSource("notify-send -a 'Envy Fan' -i " + icon + " -u " + urgency + " "
                           + q(title) + " " + q(body) + " # " + Date.now());
    }

    function handleStatus(stdout) {
        let s;
        try {
            s = JSON.parse(stdout);
        } catch (e) {
            return;
        }
        st = s;
        const now = Date.now();

        // Put the saved fan settings back whenever the driver has lost them (reboot, reload).
        if (s.driver && s.writable && !driverMatches(s) && !applyTimer.running && now - lastApply > 3000) {
            lastApply = now;
            applySettings();
        }

        // power-profiles-daemon restores performance, balanced and quiet at boot but has no
        // name for cool, so restore that one here; afterwards follow whatever else sets the mode.
        if (s.mode) {
            if (!started && cfg.hpMode === "cool" && s.mode !== "cool")
                setMode("cool");
            else
                cfg.hpMode = s.mode;
            started = true;
        }

        // HP's EC reporting a fault (fan failure, battery overheating, ...): tell the user at once.
        if (s.emergency && s.emergency !== "none" && s.emergency !== lastEmergency)
            notify("Laptop hardware emergency", "HP's controller reports: " + s.emergency.replace(/-/g, " ")
                   + ". Save your work and shut down.", "dialog-warning", "critical");
        if (s.emergency)
            lastEmergency = s.emergency;

        throttleStreak = s.state === "throttling" ? throttleStreak + 1 : 0;
        nearStreak = s.state === "near" ? nearStreak + 1 : 0;
        const where = Math.round(s.temp) + "°C, throttle point " + s.limit + "°C";
        // three polls in a row (~6 s) before throttling counts, then at most every 5 minutes
        if (cfg.notifyThrottle && throttleStreak === 3 && now - lastThrottleNote > 5 * 60000) {
            lastThrottleNote = now;
            notify("CPU is thermal throttling", where + ". Fans at " + fanSummary() + ".",
                   "temperature-warm", "critical");
        }
        if (cfg.notifyNear && nearStreak === 5 && now - lastNearNote > 10 * 60000) {
            lastNearNote = now;
            notify("CPU is close to throttling", Math.round(s.headroom) + "°C of headroom left (" + where + ").",
                   "temperature-warm", "normal");
        }
    }

    function fanSummary() {
        return st.fans && st.fans.length === 2 ? st.fans[0].duty + "% / " + st.fans[1].duty + "%" : "unknown";
    }

    P5Support.DataSource {
        id: exec
        engine: "executable"
        connectedSources: []
        onNewData: (source, data) => {
            disconnectSource(source);
            if (source === root.statusCmd) {
                root.handleStatus(data["stdout"]);
            } else if (source.indexOf(" apply ") !== -1 || source.indexOf(" mode ") !== -1) {
                try {
                    const r = JSON.parse(data["stdout"]);
                    root.lastError = r.ok ? "" : r.error;
                } catch (e) {
                    root.lastError = data["stderr"] || "helper failed";
                }
            }
        }
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: exec.connectSource(root.statusCmd)
    }

    Timer {
        id: applyTimer
        interval: 300
        onTriggered: root.applySettings()
    }

    compactRepresentation: MouseArea {
        id: compact
        hoverEnabled: true
        onClicked: root.expanded = !root.expanded

        Kirigami.Icon {
            anchors.fill: parent
            source: Plasmoid.icon
            active: compact.containsMouse
        }

        Rectangle {
            visible: root.haveTemp
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: badge.implicitWidth + 4
            height: badge.implicitHeight
            radius: 3
            color: root.stateColor

            PlasmaComponents3.Label {
                id: badge
                anchors.centerIn: parent
                text: root.haveTemp ? Math.round(root.st.temp) : ""
                font.pixelSize: Math.max(8, compact.height * 0.42)
                font.bold: true
                color: "white"
            }
        }
    }

    fullRepresentation: PlasmaExtras.Representation {
        Layout.preferredWidth: Kirigami.Units.gridUnit * 22
        Layout.preferredHeight: content.implicitHeight + Kirigami.Units.largeSpacing * 2
        Layout.minimumWidth: Kirigami.Units.gridUnit * 18

        // Everything below the fan sliders is off the bottom of a 1080p screen, and a
        // Representation does not scroll on its own, so it was simply cut off.
        PlasmaComponents3.ScrollView {
            id: scroll
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            clip: true

            ColumnLayout {
                id: content
                width: scroll.availableWidth
                spacing: Kirigami.Units.smallSpacing

                PlasmaExtras.Heading {
                    level: 4
                    visible: root.st.driver === false || root.st.writable === false || root.lastError !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.negativeTextColor
                    text: root.st.driver === false ? "The hp_envy_fan driver is not loaded."
                        : root.st.writable === false ? "The fan settings are read-only for this user (see the udev rule)."
                        : root.lastError
                }

                // Thermal headroom
                RowLayout {
                    Layout.fillWidth: true
                    PlasmaExtras.Heading {
                        level: 1
                        text: root.haveTemp ? Math.round(root.st.temp) + "°C" : "–"
                        color: root.stateColor
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        PlasmaComponents3.Label {
                            text: root.stateText
                            font.bold: true
                            color: root.stateColor
                        }
                        PlasmaComponents3.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            opacity: 0.8
                            text: root.haveTemp ? Math.round(root.st.headroom) + "°C below the " + root.st.limit + "°C throttle point" : ""
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    implicitHeight: Kirigami.Units.smallSpacing * 2
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: Kirigami.Theme.textColor
                        opacity: 0.15
                    }
                    Rectangle {
                        // 30 C at the left edge, the throttle point at the right
                        height: parent.height
                        radius: height / 2
                        color: root.stateColor
                        width: root.haveTemp
                            ? parent.width * Math.max(0.02, Math.min(1, (root.st.temp - 30) / (root.st.limit - 30)))
                            : 0
                        Behavior on width { NumberAnimation { duration: Kirigami.Units.longDuration } }
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    columns: 2
                    columnSpacing: Kirigami.Units.largeSpacing
                    rowSpacing: 0

                    Repeater {
                        model: root.st.fans ? root.st.fans : []
                        delegate: PlasmaComponents3.Label {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            text: "Fan " + (index + 1) + ": " + modelData.duty + "%"
                                  + (modelData.rpm ? "  (HP reading " + modelData.rpm + " rpm)" : "")
                        }
                    }
                    PlasmaComponents3.Label {
                        Layout.columnSpan: 2
                        opacity: 0.8
                        text: root.st.mhz_avg ? "CPU " + (root.st.mhz_avg / 1000).toFixed(1) + " GHz average, "
                                              + (root.st.load != null ? root.st.load + "% busy" : "") : ""
                    }
                }

                // Battery
                PlasmaExtras.Heading {
                    level: 5
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: !!root.st.battery && root.st.battery.health != null
                    text: "Battery"
                }
                PlasmaComponents3.Label {
                    readonly property var b: root.st.battery || {}
                    visible: b.health != null
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: b.health + "% of its original capacity, " + b.cycles + " charge cycles, made " + b.manufactured
                }
                PlasmaComponents3.Label {
                    readonly property var b: root.st.battery || {}
                    visible: b.health != null
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.8
                    text: (b.capacity != null ? b.capacity + "%, " + (b.status || "").toLowerCase() : "")
                          + (b.power_w ? ", " + b.power_w + " W" : "")
                          + (b.cells_mv && b.cells_mv.length ? " · cells " + b.cells_mv.map(v => (v / 1000).toFixed(3)).join(" / ") + " V" : "")
                }
                PlasmaComponents3.Label {
                    visible: !!root.st.emergency && root.st.emergency !== "none"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.bold: true
                    color: Kirigami.Theme.negativeTextColor
                    text: "HP's controller reports: " + (root.st.emergency || "").replace(/-/g, " ")
                }

                // HP's thermal mode
                PlasmaExtras.Heading {
                    level: 5
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: !!root.st.mode
                    text: "Power mode"
                }
                PlasmaComponents3.TabBar {
                    id: hpBar
                    readonly property var modes: ["performance", "balanced", "cool", "quiet"]
                    visible: !!root.st.mode
                    Layout.fillWidth: true
                    currentIndex: Math.max(0, modes.indexOf(root.st.mode))
                    PlasmaComponents3.TabButton {
                        text: "Performance"
                        onClicked: root.setMode("performance")
                    }
                    PlasmaComponents3.TabButton {
                        text: "Balanced"
                        onClicked: root.setMode("balanced")
                    }
                    PlasmaComponents3.TabButton {
                        text: "Cool"
                        onClicked: root.setMode("cool")
                    }
                    PlasmaComponents3.TabButton {
                        text: "Quiet"
                        onClicked: root.setMode("quiet")
                    }
                }
                PlasmaComponents3.Label {
                    visible: !!root.st.mode
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.8
                    text: {
                        switch (root.st.mode) {
                        case "performance": return "HP's performance mode: about 37 W sustained, and HP's own fans start earlier.";
                        case "balanced": return "HP's default: about 25 W sustained.";
                        case "cool": return "HP's cool mode: fans start early and power drops to about 17 W.";
                        case "quiet": return "HP's quiet mode: about 15 W and slower fans.";
                        default: return "";
                        }
                    }
                }

                // Fan mode
                PlasmaExtras.Heading {
                    level: 5
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    text: "Fans"
                }
                PlasmaComponents3.TabBar {
                    id: modeBar
                    Layout.fillWidth: true
                    currentIndex: root.cfg.mode === "auto" ? 0 : root.cfg.mode === "fixed" ? 2 : 1
                    onCurrentIndexChanged: {
                        const mode = ["auto", "curve", "fixed"][currentIndex];
                        if (root.cfg.mode !== mode) {
                            root.cfg.mode = mode;
                            applyTimer.restart();
                        }
                    }
                    PlasmaComponents3.TabButton { text: "HP auto" }
                    PlasmaComponents3.TabButton { text: "Curve" }
                    PlasmaComponents3.TabButton { text: "Fixed" }
                }

                PlasmaComponents3.Label {
                    visible: root.cfg.mode === "auto"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.8
                    text: "The laptop's own fan control, as shipped."
                }

                // Curve
                ColumnLayout {
                    visible: root.cfg.mode === "curve"
                    Layout.fillWidth: true
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.bottomMargin: Kirigami.Units.smallSpacing
                        PlasmaComponents3.CheckBox {
                            id: smoothOn
                            text: "Sit out spikes shorter than"
                            checked: root.cfg.smooth > 0
                            onToggled: {
                                root.cfg.smooth = checked ? smoothSecs.value : 0;
                                applyTimer.restart();
                            }
                        }
                        PlasmaComponents3.SpinBox {
                            id: smoothSecs
                            from: 2; to: 30
                            enabled: smoothOn.checked
                            value: root.cfg.smooth > 0 ? root.cfg.smooth : 15
                            onValueModified: {
                                if (smoothOn.checked) {
                                    root.cfg.smooth = value;
                                    applyTimer.restart();
                                }
                            }
                        }
                        PlasmaComponents3.Label {
                            text: "s"
                            enabled: smoothOn.checked
                        }
                    }

                    Canvas {
                        id: preview
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                        readonly property var deps: [root.cfg.startTemp, root.cfg.startDuty, root.cfg.fullTemp,
                                                     root.cfg.fullDuty, root.st.temp, root.cfg.limit]
                        onDepsChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d");
                            const w = width, h = height, t0 = 30, t1 = 110;
                            const x = t => (t - t0) / (t1 - t0) * w;
                            const y = d => h - 2 - d / 100 * (h - 4);
                            ctx.reset();
                            ctx.strokeStyle = Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                                      Kirigami.Theme.textColor.b, 0.2);
                            ctx.lineWidth = 1;
                            ctx.strokeRect(0.5, 0.5, w - 1, h - 1);
                            // throttle point
                            ctx.strokeStyle = Kirigami.Theme.negativeTextColor;
                            if (ctx.setLineDash)
                                ctx.setLineDash([3, 3]);
                            ctx.beginPath();
                            ctx.moveTo(x(root.cfg.limit), 0);
                            ctx.lineTo(x(root.cfg.limit), h);
                            ctx.stroke();
                            if (ctx.setLineDash)
                                ctx.setLineDash([]);
                            // the curve: HP's control below the start point
                            ctx.strokeStyle = Kirigami.Theme.highlightColor;
                            ctx.lineWidth = 2;
                            ctx.beginPath();
                            ctx.moveTo(x(root.cfg.startTemp), y(root.cfg.startDuty));
                            ctx.lineTo(x(root.cfg.fullTemp), y(root.cfg.fullDuty));
                            ctx.lineTo(w, y(root.cfg.fullDuty));
                            ctx.stroke();
                            // now
                            if (root.haveTemp) {
                                ctx.fillStyle = root.stateColor;
                                ctx.beginPath();
                                ctx.arc(x(root.st.temp), h / 2, 3, 0, 2 * Math.PI);
                                ctx.fill();
                            }
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                        columns: 2
                        rowSpacing: 0
                        columnSpacing: Kirigami.Units.largeSpacing

                        PlasmaComponents3.Label { text: "Start at " + startTemp.value + "°C" }
                        PlasmaComponents3.Label { text: "with fans at " + startDuty.value + "%" }
                        PlasmaComponents3.Slider {
                            id: startTemp
                            Layout.fillWidth: true
                            from: 40; to: 85; stepSize: 1
                            value: root.cfg.startTemp
                            onMoved: {
                                root.cfg.startTemp = value;
                                if (root.cfg.fullTemp <= value)
                                    fullTemp.value = root.cfg.fullTemp = Math.min(100, value + 5);
                                applyTimer.restart();
                            }
                        }
                        PlasmaComponents3.Slider {
                            id: startDuty
                            Layout.fillWidth: true
                            from: 20; to: 100; stepSize: 5
                            value: root.cfg.startDuty
                            onMoved: {
                                root.cfg.startDuty = value;
                                if (root.cfg.fullDuty < value)
                                    fullDuty.value = root.cfg.fullDuty = value;
                                applyTimer.restart();
                            }
                        }
                        PlasmaComponents3.Label { text: "Full at " + fullTemp.value + "°C" }
                        PlasmaComponents3.Label { text: "with fans at " + fullDuty.value + "%" }
                        PlasmaComponents3.Slider {
                            id: fullTemp
                            Layout.fillWidth: true
                            from: 45; to: 100; stepSize: 1
                            value: root.cfg.fullTemp
                            onMoved: {
                                root.cfg.fullTemp = value;
                                if (root.cfg.startTemp >= value)
                                    startTemp.value = root.cfg.startTemp = Math.max(40, value - 5);
                                applyTimer.restart();
                            }
                        }
                        PlasmaComponents3.Slider {
                            id: fullDuty
                            Layout.fillWidth: true
                            from: 20; to: 100; stepSize: 5
                            value: root.cfg.fullDuty
                            onMoved: {
                                root.cfg.fullDuty = value;
                                if (root.cfg.startDuty > value)
                                    startDuty.value = root.cfg.startDuty = value;
                                applyTimer.restart();
                            }
                        }
                    }

                    PlasmaComponents3.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: "Below the start temperature HP's control runs the fans. The fans never run slower than HP would run them."
                              + (smoothOn.checked ? " A burst of heat shorter than " + smoothSecs.value
                                 + " s is ignored; slowing down is unaffected." : "")
                    }
                }

                // Fixed
                ColumnLayout {
                    visible: root.cfg.mode === "fixed"
                    Layout.fillWidth: true
                    spacing: 0
                    PlasmaComponents3.Label {
                        text: "Fans at least " + fixedDuty.value + "%"
                    }
                    PlasmaComponents3.Slider {
                        id: fixedDuty
                        Layout.fillWidth: true
                        from: 20; to: 100; stepSize: 5
                        value: root.cfg.fixedDuty
                        onMoved: {
                            root.cfg.fixedDuty = value;
                            applyTimer.restart();
                        }
                    }
                }

                // Notifications
                PlasmaExtras.Heading {
                    level: 5
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    text: "Notifications"
                }
                PlasmaComponents3.CheckBox {
                    text: "When the CPU is throttling"
                    checked: root.cfg.notifyThrottle
                    onToggled: root.cfg.notifyThrottle = checked
                }
                RowLayout {
                    PlasmaComponents3.CheckBox {
                        text: "When it is within"
                        checked: root.cfg.notifyNear
                        onToggled: root.cfg.notifyNear = checked
                    }
                    PlasmaComponents3.SpinBox {
                        from: 3; to: 25
                        value: root.cfg.nearMargin
                        onValueModified: root.cfg.nearMargin = value
                    }
                    PlasmaComponents3.Label { text: "°C of it" }
                }
                RowLayout {
                    PlasmaComponents3.Label { text: "Throttle point" }
                    PlasmaComponents3.SpinBox {
                        from: 85; to: 110
                        value: root.cfg.limit
                        onValueModified: root.cfg.limit = value
                    }
                    PlasmaComponents3.Label { text: "°C" }
                }
            }
        }
    }
}
