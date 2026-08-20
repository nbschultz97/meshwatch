using Toybox.Attention;
using Toybox.BluetoothLowEnergy as Ble;
using Toybox.System;
using Toybox.Time;
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

    // debug HUD counters (shown on screen while we shake out the BLE flow)
    var dbgReads = 0;
    var dbgBytes = 0;
    var dbgLast = 0;
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
        try {
            Ble.setScanState(Ble.SCAN_STATE_OFF);
        } catch (e) {
        }
        if (mDevice != null) {
            try {
                Ble.unpairDevice(mDevice);
            } catch (e) {
            }
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
            // Meshtastic's fromRadio/notify require an encrypted bond; reading
            // before bonding returns STATUS_GATT_INSUFFICIENT_AUTHENTICATION.
            if (device.isBonded()) {
                beginSync(device);
            } else {
                state = S_BONDING;
                dbgKind = "bonding";
                try {
                    device.requestBond();
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
        dbgKind = "enc" + status;
        System.println("encryption status=" + status);
        if (status == Ble.STATUS_SUCCESS) {
            mDevice = device;
            beginSync(device);
        } else {
            fail("bond failed " + status);
        }
        WatchUi.requestUpdate();
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
        System.println("cccd write status=" + status);
        requestConfig();
    }

    function requestConfig() {
        state = S_SYNCING;
        try {
            mToRadio.requestWrite(Proto.encodeWantConfig(0x4e53), // "NS"
                {:writeType => Ble.WRITE_TYPE_DEFAULT});
        } catch (e) {
            fail("wantConfig: " + e.getErrorMessage());
        }
        WatchUi.requestUpdate();
    }

    function onCharacteristicWrite(characteristic, status) {
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
            dbgKind = "readErr" + status;
            System.println("read status=" + status);
            WatchUi.requestUpdate();
            return;
        }
        dbgReads++;
        if (value != null && value.size() > 0) {
            dbgBytes += value.size();
            dbgLast = value.size();
            var what = Proto.parseFromRadio(value, mStore);
            dbgKind = what.toString();
            System.println("fromRadio " + value.size() + "b -> " + what);
            if (what == :message) {
                buzz();
            }
            WatchUi.requestUpdate();
            drainFromRadio(); // keep reading until the radio returns empty
        } else {
            dbgLast = 0;
            dbgKind = "empty";
            if (state == S_SYNCING) {
                state = S_READY;
            }
            WatchUi.requestUpdate();
        }
    }

    function onCharacteristicChanged(characteristic, value) {
        // fromNum notify: new FromRadio data queued on the node
        if (state == S_READY || state == S_SYNCING) {
            drainFromRadio();
        }
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
            mStore.addMessage(0x10000002l, "rgr: " + text, false);
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

    function loadDemoData() {
        var now = Time.now().value();
        deviceName = "DEMO";
        state = S_READY;
        mStore.myNum = 0x10000001l;
        mStore.upsert(0x10000001l, {:shortName => "NOAH", :longName => "tactix 7 (you)",
            :battery => 88, :lastHeard => now});
        mStore.upsert(0x10000002l, {:shortName => "OS01", :longName => "Outstation01",
            :battery => 101, :lastHeard => now - 42, :snr => 8.5});
        mStore.upsert(0x10000003l, {:shortName => "OS02", :longName => "Outstation02 (pi12)",
            :battery => 64, :lastHeard => now - 380, :hops => 1});
        mStore.upsert(0x10000004l, {:shortName => "PCKT", :longName => "PICKET UGV",
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
