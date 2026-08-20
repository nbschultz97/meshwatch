using Toybox.Lang;
using Toybox.StringUtil;
using Toybox.System;

// Hand-rolled decoder for the handful of Meshtastic protobuf fields milestone 1
// needs. Field numbers come from meshtastic/mesh.proto and must track firmware:
//   FromRadio: 1=packet 3=my_info 4=node_info 7=config_complete_id
//   NodeInfo:  1=num 2=user 4=snr(f32) 5=last_heard(fx32) 6=device_metrics 9=hops_away
//   User:      2=long_name 3=short_name
//   DeviceMetrics: 1=battery_level
//   ToRadio:   3=want_config_id
module Proto {

    // Varints go through Long: node nums are full uint32 and overflow Number.
    // Returns [value, nextPos].
    function readVarint(b, pos) {
        var val = 0l;
        var shift = 0;
        while (pos < b.size()) {
            var byte = b[pos];
            pos++;
            val = val | ((byte & 0x7f).toLong() << shift);
            if ((byte & 0x80) == 0) {
                return [val, pos];
            }
            shift += 7;
        }
        return [val, pos];
    }

    function encodeVarint(value) {
        var out = []b;
        var v = value.toLong();
        while (true) {
            var byte = (v & 0x7f).toNumber();
            v = v >> 7;
            if (v != 0) {
                out.add(byte | 0x80);
            } else {
                out.add(byte);
                break;
            }
        }
        return out;
    }

    // ToRadio { want_config_id = nonce } — kicks the radio into streaming its
    // full state (my_info, node db, config) out of the fromRadio characteristic.
    function encodeWantConfig(nonce) {
        var out = []b;
        out.add(0x18); // field 3, varint
        out.addAll(encodeVarint(nonce));
        return out;
    }

    function bytesToString(b) {
        try {
            var s = StringUtil.utf8ArrayToString(b);
            if (s != null && s.length() > 0) {
                return s;
            }
        } catch (e) {
        }
        return "?";
    }

    // Walks one FromRadio message. Returns :complete when config_complete_id
    // arrives, :node / :myinfo / :packet / :other for logging.
    function parseFromRadio(b, store) {
        var pos = 0;
        var result = :other;
        while (pos < b.size()) {
            var r = readVarint(b, pos);
            var tag = r[0].toNumber();
            pos = r[1];
            var field = tag >> 3;
            var wire = tag & 0x7;

            if (wire == 0) {
                r = readVarint(b, pos);
                pos = r[1];
                if (field == 7) {
                    result = :complete;
                }
            } else if (wire == 2) {
                r = readVarint(b, pos);
                var len = r[0].toNumber();
                pos = r[1];
                var sub = b.slice(pos, pos + len);
                pos += len;
                if (field == 3) {
                    parseMyInfo(sub, store);
                    result = :myinfo;
                } else if (field == 4) {
                    parseNodeInfo(sub, store);
                    result = :node;
                } else if (field == 1) {
                    store.packetCount++;
                    result = :packet;
                }
            } else if (wire == 5) {
                pos += 4;
            } else if (wire == 1) {
                pos += 8;
            } else {
                break; // unknown wire type: bail rather than misparse
            }
        }
        return result;
    }

    function parseMyInfo(b, store) {
        var pos = 0;
        while (pos < b.size()) {
            var r = readVarint(b, pos);
            var tag = r[0].toNumber();
            pos = r[1];
            var field = tag >> 3;
            var wire = tag & 0x7;
            if (wire == 0) {
                r = readVarint(b, pos);
                pos = r[1];
                if (field == 1) {
                    store.myNum = r[0];
                }
            } else if (wire == 2) {
                r = readVarint(b, pos);
                pos = r[1] + r[0].toNumber();
            } else if (wire == 5) {
                pos += 4;
            } else if (wire == 1) {
                pos += 8;
            } else {
                break;
            }
        }
    }

    function parseNodeInfo(b, store) {
        var pos = 0;
        var num = null;
        var info = {};
        while (pos < b.size()) {
            var r = readVarint(b, pos);
            var tag = r[0].toNumber();
            pos = r[1];
            var field = tag >> 3;
            var wire = tag & 0x7;

            if (wire == 0) {
                r = readVarint(b, pos);
                pos = r[1];
                if (field == 1) {
                    num = r[0];
                } else if (field == 9) {
                    info.put(:hops, r[0].toNumber());
                }
            } else if (wire == 2) {
                r = readVarint(b, pos);
                var len = r[0].toNumber();
                pos = r[1];
                var sub = b.slice(pos, pos + len);
                pos += len;
                if (field == 2) {
                    parseUser(sub, info);
                } else if (field == 6) {
                    parseDeviceMetrics(sub, info);
                }
            } else if (wire == 5) {
                if (field == 4) {
                    info.put(:snr, b.decodeNumber(Lang.NUMBER_FORMAT_FLOAT,
                        {:offset => pos, :endianness => Lang.ENDIAN_LITTLE}));
                } else if (field == 5) {
                    info.put(:lastHeard, b.decodeNumber(Lang.NUMBER_FORMAT_UINT32,
                        {:offset => pos, :endianness => Lang.ENDIAN_LITTLE}));
                }
                pos += 4;
            } else if (wire == 1) {
                pos += 8;
            } else {
                break;
            }
        }
        if (num != null) {
            store.upsert(num, info);
        }
    }

    function parseUser(b, info) {
        var pos = 0;
        while (pos < b.size()) {
            var r = readVarint(b, pos);
            var tag = r[0].toNumber();
            pos = r[1];
            var field = tag >> 3;
            var wire = tag & 0x7;
            if (wire == 2) {
                r = readVarint(b, pos);
                var len = r[0].toNumber();
                pos = r[1];
                var sub = b.slice(pos, pos + len);
                pos += len;
                if (field == 2) {
                    info.put(:longName, bytesToString(sub));
                } else if (field == 3) {
                    info.put(:shortName, bytesToString(sub));
                }
            } else if (wire == 0) {
                r = readVarint(b, pos);
                pos = r[1];
            } else if (wire == 5) {
                pos += 4;
            } else if (wire == 1) {
                pos += 8;
            } else {
                break;
            }
        }
    }

    function parseDeviceMetrics(b, info) {
        var pos = 0;
        while (pos < b.size()) {
            var r = readVarint(b, pos);
            var tag = r[0].toNumber();
            pos = r[1];
            var field = tag >> 3;
            var wire = tag & 0x7;
            if (wire == 0) {
                r = readVarint(b, pos);
                pos = r[1];
                if (field == 1) {
                    info.put(:battery, r[0].toNumber());
                }
            } else if (wire == 2) {
                r = readVarint(b, pos);
                pos = r[1] + r[0].toNumber();
            } else if (wire == 5) {
                pos += 4;
            } else if (wire == 1) {
                pos += 8;
            } else {
                break;
            }
        }
    }
}
