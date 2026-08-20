using Toybox.Application;
using Toybox.WatchUi;

class HeliographApp extends Application.AppBase {
    var mClient;
    var mStore;

    function initialize() {
        AppBase.initialize();
    }

    function onStart(state) {
    }

    function onStop(state) {
        if (mClient != null) {
            mClient.shutdown();
        }
    }

    function getInitialView() {
        mStore = new NodeStore();
        mClient = new GatewayClient(mStore);
        var view = new MainView(mClient, mStore);
        mClient.start();
        return [view, new MainDelegate(view, mClient)];
    }
}
