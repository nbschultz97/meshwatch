using Toybox.Attention;
using Toybox.BluetoothLowEnergy as Ble;
using Toybox.System;
using Toybox.Time;
using Toybox.Timer;
using Toybox.WatchUi;

// BLE link to a Meshtastic node. Deliberately uses plain pairDevice() —
// CONNECTION_STRATEGY_SECURE_PAIR_BOND reboots some watches against
// PIN-disabled Meshtastic peripherals (Garmin forum bug), so don't add it.
class MeshClient extends Ble.BleDelegate {

    // Meshtastic BLE contract (firmware NimbleBluetooth.cpp)
    static const SVC_UUID       = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
    static const TORADIO_UUID   = "f75c76d2-129e-4dad-a1dd-7866124401e7";
    static const FROMRADIO_UUID = "8ba2bcc2-ee02-4a55-a531-c525c5e454d5";
    static const FROMNUM_UUID   = "ed9da18c-a800-4f66-a670-aa7547e34453";
    // Some firmware builds expose fromRadio under this id instead
    static const FROMRADIO_ALT  = "2c55e69e-4993-11ed-b878-0242ac120002";

    // states
    enum {
        S_IDLE,
        S_REGISTERING,
        S_SCANNING,
        S_CONNECTING,
        S_BONDING,
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
    var mToRadio = null;
    var mFromRadio = null;
    var mFromNum = null;
    var mSvcUuid;
    var mReadsInFlight = false;
    var mRebondTried = false;  // one automatic unpair+rebond on a read auth error
    var mSyncTimer = null;
    var mSyncAttempts = 0;     // empty-read retries while draining config

    // debug HUD counters (shown on screen while we shake out the BLE flow)
    var dbgReads = 0;
    var dbgBytes = 0;
    var dbgLast = 0;
    var dbgMax = 0;
    var dbgKind = "-";
    var mTrace = "";

    // append a step to the on-screen flow trace
    function tr(s) {
        mTrace += s;
        dbgKind = mTrace;
        WatchUi.requestUpdate();
    }

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
                    {:uuid => Ble.stringToUuid(TORADIO_UUID)},
                    {:uuid => Ble.stringToUuid(FROMRADIO_UUID)},
                    {:uuid => Ble.stringToUuid(FROMRADIO_ALT)},
                    {:uuid => Ble.stringToUuid(FROMNUM_UUID),
                     :descriptors => [Ble.cccdUuid()]}
                ]
            });
        } catch (e) {
            fail("profile: " + e.getErrorMessage());
        }
    }

    function restart() {
        if (mDevice != null) {
            try {
                Ble.unpairDevice(mDevice);
            } catch (e) {
            }
            mDevice = null;
        }
        mToRadio = null;
        mFromRadio = null;
        mFromNum = null;
        mReadsInFlight = false;
        errorMsg = null;
        startScan();
    }

    function shutdown() {
        // Keep the bond so the next launch reconnects fast; just stop scanning.
        try {
            Ble.setScanState(Ble.SCAN_STATE_OFF);
        } catch (e) {
        }
    }

    function fail(msg) {
        state = S_ERROR;
        errorMsg = msg;
        System.println("ERR " + msg);
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

    // ---- BleDelegate callbacks ----

    function onProfileRegister(uuid, status) {
        System.println("profile reg status=" + status);
        if (status == Ble.STATUS_SUCCESS) {
            startScan();
        } else {
            fail("profile reg failed " + status);
        }
    }

    function onScanResults(scanResults) {
        var r = scanResults.next();
        while (r != null) {
            var sr = r as Ble.ScanResult;
            if (isMeshtastic(sr)) {
                deviceName = sr.getDeviceName();
                System.println("found " + deviceName + " rssi=" + sr.getRssi());
                try {
                    Ble.setScanState(Ble.SCAN_STATE_OFF);
                    state = S_CONNECTING;
                    mTrace = "";
                    tr("sc");
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

    function isMeshtastic(scanResult) {
        var uuids = scanResult.getServiceUuids();
        for (var u = uuids.next(); u != null; u = uuids.next()) {
            if (u.equals(mSvcUuid)) {
                return true;
            }
        }
        var name = scanResult.getDeviceName();
        if (name != null && name.find("Meshtastic") != null) {
            return true;
        }
        return false;
    }

    function onConnectedStateChanged(device, connState) {
        System.println("conn state=" + connState);
        if (connState == Ble.CONNECTION_STATE_CONNECTED) {
            mDevice = device;
            tr("C");
            // Meshtastic's fromRadio/notify require an encrypted bond; reading
            // before bonding returns STATUS_GATT_INSUFFICIENT_AUTHENTICATION.
            if (device.isBonded()) {
                tr("b1");
                beginSync(device);
            } else {
                tr("b0");
                state = S_BONDING;
                try {
                    device.requestBond();
                    tr("rq");
                } catch (e) {
                    fail("bond: " + e.getErrorMessage());
                }
            }
        } else {
            // dropped: node rebooted or walked out of range — rescan
            mToRadio = null;
            mFromRadio = null;
            mFromNum = null;
            mReadsInFlight = false;
            if (state != S_IDLE && state != S_ERROR) {
                startScan();
            }
        }
        WatchUi.requestUpdate();
    }

    // Called after requestBond() completes (or a prior bond re-established).
    function onEncryptionStatus(device, status) {
        tr("e" + status);
        System.println("encryption status=" + status);
        if (status == Ble.STATUS_SUCCESS) {
            mDevice = device;
            beginSync(device);
        } else {
            fail("bond failed " + status);
        }
    }

    // Bonded and ready: grab characteristics, enable notify, request config.
    function beginSync(device) {
        var svc = device.getService(mSvcUuid);
        if (svc == null) {
            fail("no meshtastic svc");
            return;
        }
        mToRadio = svc.getCharacteristic(Ble.stringToUuid(TORADIO_UUID));
        mFromRadio = svc.getCharacteristic(Ble.stringToUuid(FROMRADIO_UUID));
        if (mFromRadio == null) {
            mFromRadio = svc.getCharacteristic(Ble.stringToUuid(FROMRADIO_ALT));
        }
        mFromNum = svc.getCharacteristic(Ble.stringToUuid(FROMNUM_UUID));
        if (mToRadio == null || mFromRadio == null) {
            fail("chars missing");
            return;
        }
        tr("S");
        state = S_ENABLING;
        if (mFromNum != null) {
            try {
                var cccd = mFromNum.getDescriptor(Ble.cccdUuid());
                cccd.requestWrite([0x01, 0x00]b);
            } catch (e) {
                requestConfig(); // notify is an optimization; press on
            }
        } else {
            requestConfig();
        }
        WatchUi.requestUpdate();
    }

    function onDescriptorWrite(descriptor, status) {
        tr("d" + status);
        System.println("cccd write status=" + status);
        requestConfig();
    }

    function requestConfig() {
        state = S_SYNCING;
        mSyncAttempts = 0;
        try {
            mToRadio.requestWrite(Proto.encodeWantConfig(0x4e53), // "NS"
                {:writeType => Ble.WRITE_TYPE_DEFAULT});
        } catch (e) {
            fail("wantConfig: " + e.getErrorMessage());
        }
        WatchUi.requestUpdate();
    }

    // The node queues its config dump a few ms after want_config; an immediate
    // read can beat it and return empty. Retry a handful of times before
    // concluding the stream is drained.
    function scheduleRetry() {
        if (mSyncTimer == null) {
            mSyncTimer = new Timer.Timer();
        }
        mSyncTimer.start(method(:onSyncRetry), 300, false);
    }

    function onSyncRetry() as Void {
        drainFromRadio();
    }

    function onCharacteristicWrite(characteristic, status) {
        tr("w" + status);
        System.println("char write status=" + status);
        if (status == Ble.STATUS_SUCCESS) {
            drainFromRadio();
        } else {
            fail("toRadio write " + status);
        }
    }

    function drainFromRadio() {
        if (mFromRadio == null || mReadsInFlight) {
            return;
        }
        try {
            mReadsInFlight = true;
            mFromRadio.requestRead();
        } catch (e) {
            mReadsInFlight = false;
        }
    }

    function onCharacteristicRead(characteristic, status, value) {
        mReadsInFlight = false;
        if (status != Ble.STATUS_SUCCESS) {
            tr("r" + status);
            System.println("read status=" + status);
            // stale bond from a prior pairing mode: forget it and re-bond once
            if (!mRebondTried) {
                mRebondTried = true;
                tr("RB");
                forceRebond();
            }
            return;
        }
        if (value != null && value.size() > 0) {
            dbgReads++;
            dbgBytes += value.size();
            dbgLast = value.size();
            if (value.size() > dbgMax) { dbgMax = value.size(); }
            mSyncAttempts = 0; // got data; reset the empty-read budget
            var what = :other;
            try {
                what = Proto.parseFromRadio(value, mStore);
            } catch (e) {
                tr("PX"); // parse threw: skip this packet, keep draining
            }
            System.println("fromRadio " + value.size() + "b -> " + what);
            if (what == :message) {
                buzz();
            }
            if (what == :complete) {
                tr("done");
                state = S_READY;
                mRebondTried = false;
            } else {
                drainFromRadio(); // more packets queued; read the next now
            }
            WatchUi.requestUpdate();
        } else {
            dbgLast = 0;
            // empty during sync: node may not have queued yet — retry briefly
            if (state == S_SYNCING && mSyncAttempts < 25) {
                mSyncAttempts++;
                scheduleRetry();
            }
            WatchUi.requestUpdate();
        }
    }

    function onCharacteristicChanged(characteristic, value) {
        // fromNum notify: new FromRadio data queued on the node
        tr("N");
        if (state == S_READY || state == S_SYNCING) {
            drainFromRadio();
        }
    }

    // Drop the current (possibly stale) bond and scan again for a fresh pair.
    function forceRebond() {
        try {
            if (mDevice != null) {
                Ble.unpairDevice(mDevice);
            }
        } catch (e) {
        }
        mDevice = null;
        mToRadio = null;
        mFromRadio = null;
        mFromNum = null;
        mReadsInFlight = false;
        startScan();
    }

    function buzz() {
        if (Attention has :vibrate) {
            Attention.vibrate([new Attention.VibeProfile(75, 400)]);
        }
    }

    // Broadcasts a text on the primary channel. Returns a short status
    // string for the UI.
    function sendText(text) {
        if (deviceName != null && deviceName.equals("DEMO")) {
            mStore.addMessage(mStore.myNum, text, true);
            mStore.addMessage(0xA0000002l, "rgr: " + text, false);
            buzz();
            WatchUi.requestUpdate();
            return "sent (demo)";
        }
        if (state != S_READY || mToRadio == null) {
            return "no link";
        }
        try {
            mToRadio.requestWrite(Proto.encodeTextMessage(text),
                {:writeType => Ble.WRITE_TYPE_DEFAULT});
            mStore.addMessage(mStore.myNum, text, true);
            WatchUi.requestUpdate();
            return "sending";
        } catch (e) {
            return "send failed";
        }
    }

    // Pre-launch demo roster. The watch shows this for the first few seconds
    // before a real Meshtastic node DB has come in over BLE — gives the user
    // something on screen so the UI isn't blank. Replaced the moment a real
    // config-complete handshake lands. Nodes and IDs are generic on purpose;
    // they are placeholders, not anyone's actual mesh.
    function loadDemoData() {
        var now = Time.now().value();
        deviceName = "DEMO";
        state = S_READY;
        mStore.myNum = 0xA0000001l;
        mStore.upsert(0xA0000001l, {:shortName => "WATCH", :longName => "your watch (demo)",
            :battery => 88, :lastHeard => now});
        mStore.upsert(0xA0000002l, {:shortName => "ALPH", :longName => "alpha-node",
            :battery => 101, :lastHeard => now - 42, :snr => 8.5});
        mStore.upsert(0xA0000003l, {:shortName => "BRAV", :longName => "bravo-node",
            :battery => 64, :lastHeard => now - 380, :hops => 1});
        mStore.upsert(0xA0000004l, {:shortName => "CHAR", :longName => "charlie-node",
            :battery => 77, :lastHeard => now - 7200, :hops => 2});
        mStore.packetCount = 17;
        WatchUi.requestUpdate();
    }

    function stateText() {
        if (state == S_REGISTERING) { return "starting BLE"; }
        if (state == S_SCANNING)    { return "scanning..."; }
        if (state == S_CONNECTING)  { return "connecting"; }
        if (state == S_BONDING)     { return "bonding"; }
        if (state == S_ENABLING)    { return "enabling notify"; }
        if (state == S_SYNCING)     { return "syncing nodes"; }
        if (state == S_READY)       { return "live"; }
        if (state == S_ERROR)       { return errorMsg == null ? "error" : errorMsg; }
        return "idle";
    }
}
