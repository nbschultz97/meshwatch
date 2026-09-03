using Toybox.Attention;
using Toybox.BluetoothLowEnergy as Ble;
using Toybox.System;
using Toybox.Time;
using Toybox.WatchUi;

// Client for the Heliograph gateway's watch-friendly BLE service. Every frame
// is <= 20 bytes so nothing is truncated by the tactix's 22-byte read cap.
// Same public surface as the old MeshClient so the UI is unchanged.
class GatewayClient extends Ble.BleDelegate {

    static const SVC_UUID = "a3c8f000-7b1e-4c9a-9f0e-1234567890ab";
    static const TX_UUID  = "a3c8f001-7b1e-4c9a-9f0e-1234567890ab";
    static const RX_UUID  = "a3c8f002-7b1e-4c9a-9f0e-1234567890ab";

    enum {
        S_IDLE,
        S_REGISTERING,
        S_SCANNING,
        S_CONNECTING,
        S_ENABLING,
        S_SYNCING,
        S_READY,
        S_ERROR
    }

    var state = S_IDLE;
    var errorMsg = null;
    var deviceName = null;

    var mStore;
    var mDevice = null;
    var mTx = null;
    var mRx = null;
    var mSvcUuid;
    var mReadsInFlight = false;
    var mMsgParts = {};   // msg id -> accumulated text
    var mImgBuf = null;   // 2048B = 64x64 4-bit grayscale
    var mImgSeen = 0;
    var mImgReady = false; // set when a fresh image finishes arriving
    var mImgVersion = 0;   // bumps each completed image (cache invalidation)

    // debug HUD
    var dbgReads = 0;
    var dbgBytes = 0;
    var dbgLast = 0;
    var dbgMax = 0;
    var dbgKind = "-";

    function initialize(store) {
        BleDelegate.initialize();
        mStore = store;
        mSvcUuid = Ble.stringToUuid(SVC_UUID);
    }

    function start() {
        Ble.setDelegate(self);
        state = S_REGISTERING;
        try {
            Ble.registerProfile({
                :uuid => mSvcUuid,
                :characteristics => [
                    {:uuid => Ble.stringToUuid(TX_UUID),
                     :descriptors => [Ble.cccdUuid()]},
                    {:uuid => Ble.stringToUuid(RX_UUID)}
                ]
            });
        } catch (e) {
            fail("profile: " + e.getErrorMessage());
        }
    }

    function restart() {
        mTx = null;
        mRx = null;
        mReadsInFlight = false;
        errorMsg = null;
        startScan();
    }

    function shutdown() {
        try {
            Ble.setScanState(Ble.SCAN_STATE_OFF);
        } catch (e) {
        }
    }

    function fail(msg) {
        state = S_ERROR;
        errorMsg = msg;
        WatchUi.requestUpdate();
    }

    function startScan() {
        state = S_SCANNING;
        try {
            Ble.setScanState(Ble.SCAN_STATE_SCANNING);
        } catch (e) {
            fail("scan: " + e.getErrorMessage());
        }
        WatchUi.requestUpdate();
    }

    function onProfileRegister(uuid, status) {
        if (status == Ble.STATUS_SUCCESS) {
            startScan();
        } else {
            fail("profile reg " + status);
        }
    }

    function onScanResults(scanResults) {
        var r = scanResults.next();
        while (r != null) {
            var sr = r as Ble.ScanResult;
            if (matches(sr)) {
                deviceName = sr.getDeviceName();
                try {
                    Ble.setScanState(Ble.SCAN_STATE_OFF);
                    state = S_CONNECTING;
                    mDevice = Ble.pairDevice(sr);
                } catch (e) {
                    fail("pair: " + e.getErrorMessage());
                }
                WatchUi.requestUpdate();
                return;
            }
            r = scanResults.next();
        }
    }

    function matches(sr) {
        var uuids = sr.getServiceUuids();
        for (var u = uuids.next(); u != null; u = uuids.next()) {
            if (u.equals(mSvcUuid)) {
                return true;
            }
        }
        var name = sr.getDeviceName();
        return name != null && name.find("Heliograph") != null;
    }

    function onConnectedStateChanged(device, connState) {
        if (connState == Ble.CONNECTION_STATE_CONNECTED) {
            mDevice = device;
            var svc = device.getService(mSvcUuid);
            if (svc == null) {
                fail("no gw svc");
                return;
            }
            mTx = svc.getCharacteristic(Ble.stringToUuid(TX_UUID));
            mRx = svc.getCharacteristic(Ble.stringToUuid(RX_UUID));
            if (mTx == null || mRx == null) {
                fail("chars missing");
                return;
            }
            state = S_ENABLING;
            try {
                var cccd = mTx.getDescriptor(Ble.cccdUuid());
                cccd.requestWrite([0x01, 0x00]b);
            } catch (e) {
                requestSync();
            }
        } else {
            mTx = null;
            mRx = null;
            mReadsInFlight = false;
            if (state != S_IDLE && state != S_ERROR) {
                startScan();
            }
        }
        WatchUi.requestUpdate();
    }

    function onDescriptorWrite(descriptor, status) {
        requestSync();
    }

    // ask the gateway to (re)queue all nodes + messages
    function requestSync() {
        state = S_SYNCING;
        try {
            mRx.requestWrite([0x01]b, {:writeType => Ble.WRITE_TYPE_DEFAULT});
        } catch (e) {
            fail("sync: " + e.getErrorMessage());
        }
        WatchUi.requestUpdate();
    }

    function onCharacteristicWrite(characteristic, status) {
        // after a SYNC or SEND write, pull whatever the gateway queued
        drain();
    }

    function drain() {
        if (mTx == null || mReadsInFlight) {
            return;
        }
        try {
            mReadsInFlight = true;
            mTx.requestRead();
        } catch (e) {
            mReadsInFlight = false;
        }
    }

    function onCharacteristicRead(characteristic, status, value) {
        mReadsInFlight = false;
        if (status != Ble.STATUS_SUCCESS) {
            dbgKind = "rd" + status;
            WatchUi.requestUpdate();
            return;
        }
        if (value == null || value.size() == 0) {
            return;
        }
        dbgReads++;
        dbgBytes += value.size();
        dbgLast = value.size();
        if (value.size() > dbgMax) { dbgMax = value.size(); }

        var type = value[0];
        if (type == 0x00) {            // END
            dbgKind = "done";
            if (state == S_SYNCING) {
                state = S_READY;
            }
        } else if (type == 0x01) {     // NODE
            parseNode(value);
            dbgKind = "node";
            drain();
        } else if (type == 0x02) {     // MSG chunk
            parseMsg(value);
            dbgKind = "msg";
            drain();
        } else if (type == 0x03) {     // IMAGE chunk
            parseImage(value);
            dbgKind = "img" + mImgSeen;
            drain();
        } else {
            dbgKind = "?" + type;
            drain();
        }
        WatchUi.requestUpdate();
    }

    function parseNode(v) {
        if (v.size() < 7) { return; }
        var num = v[1].toLong()
            | (v[2].toLong() << 8)
            | (v[3].toLong() << 16)
            | (v[4].toLong() << 24);
        var batt = v[5];
        var flags = v[6];
        var name = "";
        for (var i = 7; i < v.size(); i++) {
            var c = v[i] & 0xff;
            if (c == 0) { break; }
            if (c >= 32 && c < 127) { name += c.toChar().toString(); }
        }
        var info = {:shortName => name, :battery => batt,
            :lastHeard => Time.now().value()};
        mStore.upsert(num, info);
        if ((flags & 0x01) != 0) {
            mStore.myNum = num;
        }
    }

    function parseMsg(v) {
        if (v.size() < 3) { return; }
        var id = v[1];
        var seq = v[2];
        var last = (seq & 0x80) != 0;
        var text = "";
        for (var i = 3; i < v.size(); i++) {
            var c = v[i] & 0xff;
            if (c >= 32 && c < 127) { text += c.toChar().toString(); }
        }
        var acc = mMsgParts.get(id);
        if (acc == null) { acc = ""; }
        acc += text;
        mMsgParts.put(id, acc);
        if (last) {
            mStore.addMessage(null, acc, false);
            mMsgParts.remove(id);
            buzz();
        }
    }

    function parseImage(v) {
        if (v.size() < 3) { return; }
        var seq = v[1];
        if (mImgBuf == null) {
            mImgBuf = new [2048]b;
            mImgSeen = 0;
        }
        var base = seq * 16;
        for (var i = 0; i < 16 && (base + i) < 2048 && (2 + i) < v.size(); i++) {
            mImgBuf[base + i] = v[2 + i];
        }
        mImgSeen++;
        if (seq == 127) {      // last chunk of a 128-frame image
            mImgReady = true;
            mImgVersion++;
            buzz();
        }
    }

    // ask the gateway to stream the latest detection thumbnail
    function getImage() {
        mImgBuf = null;
        mImgSeen = 0;
        mImgReady = false;
        if (state == S_READY && mRx != null) {
            try {
                mRx.requestWrite([0x03]b, {:writeType => Ble.WRITE_TYPE_DEFAULT});
            } catch (e) {
            }
        }
    }

    function onCharacteristicChanged(characteristic, value) {
        drain();
    }

    function buzz() {
        if (Attention has :vibrate) {
            Attention.vibrate([new Attention.VibeProfile(75, 400)]);
        }
    }

    function sendText(text) {
        if (deviceName != null && deviceName.equals("DEMO")) {
            mStore.addMessage(mStore.myNum, text, true);
            return "sent (demo)";
        }
        if (state != S_READY || mRx == null) {
            return "no link";
        }
        try {
            var payload = [0x02]b;
            var chars = text.toUtf8Array();
            for (var i = 0; i < chars.size() && payload.size() < 20; i++) {
                payload.add(chars[i]);
            }
            mRx.requestWrite(payload, {:writeType => Ble.WRITE_TYPE_DEFAULT});
            mStore.addMessage(mStore.myNum, text, true);
            return "sent";
        } catch (e) {
            return "send failed";
        }
    }

    // Pre-launch demo roster. See MeshClient.loadDemoData() — same idea,
    // generic placeholders, replaced on first real node-DB sync.
    function loadDemoData() {
        var now = Time.now().value();
        deviceName = "DEMO";
        state = S_READY;
        mStore.myNum = 0xA0000001l;
        mStore.upsert(0xA0000001l, {:shortName => "WATCH", :battery => 88, :lastHeard => now});
        mStore.upsert(0xA0000002l, {:shortName => "ALPH", :battery => 101, :lastHeard => now - 42});
        mStore.addMessage(null, "ALPH: hello from a demo node", false);
        WatchUi.requestUpdate();
    }

    function stateText() {
        if (state == S_REGISTERING) { return "starting BLE"; }
        if (state == S_SCANNING)    { return "scanning..."; }
        if (state == S_CONNECTING)  { return "connecting"; }
        if (state == S_ENABLING)    { return "enabling notify"; }
        if (state == S_SYNCING)     { return "syncing"; }
        if (state == S_READY)       { return "live"; }
        if (state == S_ERROR)       { return errorMsg == null ? "error" : errorMsg; }
        return "idle";
    }
}
