using Toybox.BluetoothLowEnergy as Ble;
using Toybox.System;
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
        for (var r = scanResults.next(); r != null; r = scanResults.next()) {
            if (isMeshtastic(r)) {
                deviceName = r.getDeviceName();
                System.println("found " + deviceName + " rssi=" + r.getRssi());
                try {
                    Ble.setScanState(Ble.SCAN_STATE_OFF);
                    state = S_CONNECTING;
                    mDevice = Ble.pairDevice(r);
                } catch (e) {
                    fail("pair: " + e.getErrorMessage());
                }
                WatchUi.requestUpdate();
                return;
            }
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
                    // notifications are an optimization; go straight to config
                    requestConfig();
                }
            } else {
                requestConfig();
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
            System.println("read status=" + status);
            return;
        }
        if (value != null && value.size() > 0) {
            var what = Proto.parseFromRadio(value, mStore);
            System.println("fromRadio " + value.size() + "b -> " + what);
            WatchUi.requestUpdate();
            drainFromRadio(); // keep reading until the radio returns empty
        } else {
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

    function stateText() {
        if (state == S_REGISTERING) { return "starting BLE"; }
        if (state == S_SCANNING)    { return "scanning..."; }
        if (state == S_CONNECTING)  { return "connecting"; }
        if (state == S_ENABLING)    { return "enabling notify"; }
        if (state == S_SYNCING)     { return "syncing nodes"; }
        if (state == S_READY)       { return "live"; }
        if (state == S_ERROR)       { return errorMsg == null ? "error" : errorMsg; }
        return "idle";
    }
}
