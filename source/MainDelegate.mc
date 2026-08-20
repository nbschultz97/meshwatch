using Toybox.WatchUi;

class MainDelegate extends WatchUi.BehaviorDelegate {
    var mView;
    var mClient;

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

    // START/enter: retry from error, or force a rescan
    function onSelect() {
        mClient.restart();
        return true;
    }
}
