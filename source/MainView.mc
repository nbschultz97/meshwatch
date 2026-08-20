using Toybox.WatchUi;
using Toybox.Graphics;
using Toybox.System;
using Toybox.Time;
using Toybox.Timer;

class MainView extends WatchUi.View {
    var mClient;
    var mStore;
    var mTimer;
    var scrollOfs = 0;
    var mode = :nodes;   // :nodes | :msgs | :image
    var flash = null;    // transient status line (e.g. send result)
    var flashUntil = 0;
    var mBmp = null;     // cached rendered photo
    var mBmpVersion = -1;

    const ROW_H = 52;
    const AMBER = 0xFFB020;

    function initialize(client, store) {
        View.initialize();
        mClient = client;
        mStore = store;
    }

    function onShow() {
        mTimer = new Timer.Timer();
        mTimer.start(method(:onTick), 1000, true);
    }

    function onHide() {
        if (mTimer != null) {
            mTimer.stop();
            mTimer = null;
        }
    }

    function onTick() as Void {
        WatchUi.requestUpdate();
    }

    function toggleMode() {
        if (mode == :nodes) {
            mode = :msgs;
            mStore.unread = 0;
        } else if (mode == :msgs) {
            mode = :image;
        } else {
            mode = :nodes;
        }
        scrollOfs = 0;
        WatchUi.requestUpdate();
    }

    function showFlash(text) {
        flash = text;
        flashUntil = Time.now().value() + 3;
        WatchUi.requestUpdate();
    }

    function scroll(delta) {
        scrollOfs += delta;
        var max = ((mode == :nodes) ? mStore.count() : mStore.messages.size()) - 1;
        if (max < 0) {
            max = 0;
        }
        if (scrollOfs < 0) {
            scrollOfs = 0;
        }
        if (scrollOfs > max) {
            scrollOfs = max;
        }
        WatchUi.requestUpdate();
    }

    function onUpdate(dc) {
        var w = dc.getWidth();
        var h = dc.getHeight();
        var now = Time.now().value();
        dc.setColor(Graphics.COLOR_BLACK, Graphics.COLOR_BLACK);
        dc.clear();

        if (mClient.mImgReady && mode != :image) {
            mode = :image;       // a fresh photo just finished arriving
            mClient.mImgReady = false;
        }

        var title = "HELIOGRAPH";
        if (mode == :msgs) { title = "MESSAGES"; }
        else if (mode == :image) { title = "PHOTO"; }
        dc.setColor(AMBER, Graphics.COLOR_TRANSPARENT);
        dc.drawText(w / 2, 26, Graphics.FONT_TINY, title,
            Graphics.TEXT_JUSTIFY_CENTER);

        var status;
        if (flash != null && now < flashUntil) {
            status = flash;
        } else {
            status = mClient.stateText();
            if (mClient.deviceName != null && mClient.state == mClient.S_READY) {
                status = mClient.deviceName + "  (" + status + ")";
            }
            if (mStore.unread > 0 && mode == :nodes) {
                status = "[" + mStore.unread + " new] " + status;
            }
        }
        dc.setColor(mClient.state == mClient.S_READY
                ? Graphics.COLOR_GREEN : Graphics.COLOR_LT_GRAY,
            Graphics.COLOR_TRANSPARENT);
        dc.drawText(w / 2, 58, Graphics.FONT_XTINY, status,
            Graphics.TEXT_JUSTIFY_CENTER);

        if (mode == :nodes) {
            drawNodes(dc, w, h, now);
        } else if (mode == :msgs) {
            drawMessages(dc, w, h, now);
        } else {
            drawImage(dc, w, h);
        }

        if (mode != :image) {
            // kept in the wide center band; a round display clips wide text at
            // the very top/bottom edges
            dc.setColor(Graphics.COLOR_DK_GRAY, Graphics.COLOR_TRANSPARENT);
            dc.drawText(w / 2, h / 2 + 66, Graphics.FONT_XTINY,
                (mode == :nodes)
                    ? mStore.count() + " nodes  " + mStore.messages.size() + " msgs"
                    : "START to send",
                Graphics.TEXT_JUSTIFY_CENTER);

            // debug HUD: reads / total bytes / max read / last parse kind
            dc.setColor(0x00AAFF, Graphics.COLOR_TRANSPARENT);
            dc.drawText(w / 2, h / 2 + 96, Graphics.FONT_XTINY,
                "rd" + mClient.dbgReads + " b" + mClient.dbgBytes
                    + " mx" + mClient.dbgMax + " " + mClient.dbgKind,
                Graphics.TEXT_JUSTIFY_CENTER);
        }
    }

    function drawNodes(dc, w, h, now) {
        var top = 96;
        var visible = (h - top - 30) / ROW_H;
        var n = mStore.count();

        if (n == 0) {
            dc.setColor(Graphics.COLOR_DK_GRAY, Graphics.COLOR_TRANSPARENT);
            dc.drawText(w / 2, h / 2, Graphics.FONT_TINY, "no nodes yet",
                Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
            return;
        }

        for (var i = 0; i < visible; i++) {
            var idx = scrollOfs + i;
            if (idx >= n) {
                break;
            }
            var node = mStore.get(idx);
            var y = top + i * ROW_H;

            var shortName = node.get(:shortName);
            if (shortName == null) {
                shortName = "----";
            }
            var isMe = mStore.myNum != null
                && mStore.numAt(idx).equals(mStore.myNum);

            dc.setColor(isMe ? AMBER : Graphics.COLOR_WHITE,
                Graphics.COLOR_TRANSPARENT);
            dc.drawText(70, y, Graphics.FONT_SMALL, shortName,
                Graphics.TEXT_JUSTIFY_LEFT);

            var detail = "";
            var batt = node.get(:battery);
            if (batt != null) {
                detail += (batt > 100 ? "PWR" : batt + "%");
            }
            var lastHeard = node.get(:lastHeard);
            if (lastHeard != null && lastHeard.toNumber() > 0) {
                detail += "  " + ageText(now - lastHeard.toNumber());
            }
            var hops = node.get(:hops);
            if (hops != null && hops > 0) {
                detail += "  " + hops + "hop";
            }
            dc.setColor(Graphics.COLOR_LT_GRAY, Graphics.COLOR_TRANSPARENT);
            dc.drawText(w - 60, y + 6, Graphics.FONT_XTINY, detail,
                Graphics.TEXT_JUSTIFY_RIGHT);

            var longName = node.get(:longName);
            if (longName != null) {
                dc.setColor(Graphics.COLOR_DK_GRAY, Graphics.COLOR_TRANSPARENT);
                dc.drawText(70, y + 26, Graphics.FONT_XTINY, longName,
                    Graphics.TEXT_JUSTIFY_LEFT);
            }
        }
    }

    function drawMessages(dc, w, h, now) {
        var top = 96;
        var visible = (h - top - 30) / ROW_H;
        var msgs = mStore.messages;

        if (msgs.size() == 0) {
            dc.setColor(Graphics.COLOR_DK_GRAY, Graphics.COLOR_TRANSPARENT);
            dc.drawText(w / 2, h / 2, Graphics.FONT_TINY, "no messages",
                Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
            return;
        }

        for (var i = 0; i < visible; i++) {
            var idx = scrollOfs + i;
            if (idx >= msgs.size()) {
                break;
            }
            var m = msgs[idx];
            var y = top + i * ROW_H;
            var out = m.get(:out) == true;
            var who = out ? "you" : mStore.nameFor(m.get(:from));

            dc.setColor(out ? Graphics.COLOR_DK_GRAY : AMBER,
                Graphics.COLOR_TRANSPARENT);
            dc.drawText(70, y, Graphics.FONT_XTINY,
                who + "  " + ageText(now - m.get(:ts)),
                Graphics.TEXT_JUSTIFY_LEFT);

            dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_TRANSPARENT);
            dc.drawText(70, y + 20, Graphics.FONT_SMALL, m.get(:text),
                Graphics.TEXT_JUSTIFY_LEFT);
        }
    }

    function drawImage(dc, w, h) {
        // heavy render happens once per image, into a cached bitmap; the
        // per-frame path is a single scaled blit (avoids the watchdog).
        if (mClient.mImgVersion <= 0) {
            dc.setColor(Graphics.COLOR_DK_GRAY, Graphics.COLOR_TRANSPARENT);
            var msg = "no photo (START menu)";
            if (mClient.mImgSeen > 0) {
                msg = "receiving " + (mClient.mImgSeen * 100 / 128) + "%";
            }
            dc.drawText(w / 2, h / 2, Graphics.FONT_TINY, msg,
                Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
            return;
        }
        if (mBmpVersion != mClient.mImgVersion) {
            buildBitmap();
            mBmpVersion = mClient.mImgVersion;
        }
        var side = 320;
        if (mBmp != null) {
            dc.drawScaledBitmap((w - side) / 2, 88, side, side, mBmp);
        }
    }

    // Render the 64x64 4-bit buffer into a native bitmap once.
    function buildBitmap() {
        var buf = mClient.mImgBuf;
        if (buf == null || buf.size() < 2048) {
            return;
        }
        var ref = Graphics.createBufferedBitmap({:width => 64, :height => 64});
        mBmp = ref.get();
        var bdc = mBmp.getDc();
        for (var y = 0; y < 64; y++) {
            var rowbase = y * 32;
            for (var x = 0; x < 64; x++) {
                var byte = buf[rowbase + (x / 2)];
                var nib = (x % 2 == 0) ? ((byte >> 4) & 0x0f) : (byte & 0x0f);
                var g = nib * 17;
                bdc.setColor((g << 16) | (g << 8) | g, Graphics.COLOR_TRANSPARENT);
                bdc.drawPoint(x, y);
            }
        }
    }

    function ageText(secs) {
        if (secs < 0) {
            secs = 0;
        }
        if (secs < 60) {
            return secs + "s";
        }
        if (secs < 3600) {
            return (secs / 60) + "m";
        }
        if (secs < 86400) {
            return (secs / 3600) + "h";
        }
        return (secs / 86400) + "d";
    }
}
