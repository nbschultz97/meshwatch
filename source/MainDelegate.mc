using Toybox.WatchUi;

class MainDelegate extends WatchUi.BehaviorDelegate {
    var mView;
    var mClient;

    // canned messages: short on purpose — CIQ BLE writes are size-limited
    static const CANNED = ["ACK", "RGR", "OTW", "RTB", "HOLD", "SOS"];

    function initialize(view, client) {
        BehaviorDelegate.initialize();
        mView = view;
        mClient = client;
    }

    function onNextPage() {
        mView.scroll(1);
        return true;
    }

    function onPreviousPage() {
        mView.scroll(-1);
        return true;
    }

    // START: open the send menu
    function onSelect() {
        var menu = new WatchUi.Menu2({:title => "Send"});
        for (var i = 0; i < CANNED.size(); i++) {
            menu.addItem(new WatchUi.MenuItem(CANNED[i], null, CANNED[i], {}));
        }
        menu.addItem(new WatchUi.MenuItem("Type...", null, :type, {}));
        menu.addItem(new WatchUi.MenuItem("Rescan", null, :rescan, {}));
        menu.addItem(new WatchUi.MenuItem("Demo data", null, :demo, {}));
        WatchUi.pushView(menu, new SendMenuDelegate(mView, mClient),
            WatchUi.SLIDE_UP);
        return true;
    }

    // MENU (long-press UP): flip between node list and messages
    function onMenu() {
        mView.toggleMode();
        return true;
    }
}

class SendMenuDelegate extends WatchUi.Menu2InputDelegate {
    var mView;
    var mClient;

    function initialize(view, client) {
        Menu2InputDelegate.initialize();
        mView = view;
        mClient = client;
    }

    function onSelect(item) {
        var id = item.getId();
        WatchUi.popView(WatchUi.SLIDE_DOWN);
        if (id == :demo) {
            mClient.loadDemoData();
            mView.showFlash("demo loaded");
        } else if (id == :rescan) {
            mClient.restart();
            mView.showFlash("rescanning");
        } else if (id == :type) {
            WatchUi.pushView(new WatchUi.TextPicker(""),
                new TypeDelegate(mView, mClient), WatchUi.SLIDE_LEFT);
        } else {
            mView.showFlash(mClient.sendText(id));
        }
    }
}

// On-screen keyboard for freeform messages.
class TypeDelegate extends WatchUi.TextPickerDelegate {
    var mView;
    var mClient;

    function initialize(view, client) {
        TextPickerDelegate.initialize();
        mView = view;
        mClient = client;
    }

    function onTextEntered(text, changed) {
        if (text != null && text.length() > 0) {
            mView.showFlash(mClient.sendText(text));
        }
        return true;
    }

    function onCancel() {
        return true;
    }
}
