"""PC-side mesh witness: connects to the Heltec over serial and logs every
text message and node it sees. Run while testing Heliograph on the watch so
we can prove the watch -> node -> mesh path from the other side."""
import sys
import time
from pubsub import pub
import meshtastic.serial_interface


def on_receive(packet, interface):
    try:
        d = packet.get("decoded", {})
        port = d.get("portnum")
        frm = packet.get("fromId") or packet.get("from")
        if port == "TEXT_MESSAGE_APP":
            txt = d.get("payload", b"")
            if isinstance(txt, bytes):
                txt = txt.decode("utf-8", "replace")
            print("[{}] TEXT from {}: {}".format(
                time.strftime("%H:%M:%S"), frm, txt), flush=True)
        else:
            print("[{}] pkt from {} port={}".format(
                time.strftime("%H:%M:%S"), frm, port), flush=True)
    except Exception as e:
        print("err", e, flush=True)


def on_conn(interface, topic=pub.AUTO_TOPIC):
    print("witness connected to node, listening...", flush=True)


pub.subscribe(on_receive, "meshtastic.receive")
pub.subscribe(on_conn, "meshtastic.connection.established")
port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
iface = meshtastic.serial_interface.SerialInterface(devPath=port)
while True:
    time.sleep(1)
