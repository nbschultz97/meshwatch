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

    // ToRadio { packet: MeshPacket { to: broadcast, decoded: Data {
    // portnum: TEXT_MESSAGE_APP, payload: text }}}. id/want_ack omitted to
    // stay under CIQ's small BLE write limit; firmware assigns the id.
    function encodeTextMessage(text) {
        var payload = []b;
        var chars = text.toUtf8Array();
        for (var i = 0; i < chars.size(); i++) {
            payload.add(chars[i]);
        }
        var data = []b;
        data.add(0x08); // Data.portnum, varint
        data.add(0x01); // TEXT_MESSAGE_APP
        data.add(0x12); // Data.payload, bytes
        data.addAll(encodeVarint(payload.size()));
        data.addAll(payload);

        var mesh = []b;
        mesh.add(0x15); // MeshPacket.to, fixed32
        mesh.addAll([0xff, 0xff, 0xff, 0xff]b); // broadcast
        mesh.add(0x22); // MeshPacket.decoded, message
        mesh.addAll(encodeVarint(data.size()));
        mesh.addAll(data);

        var out = []b;
        out.add(0x0a); // ToRadio.packet, message
        out.addAll(encodeVarint(mesh.size()));
        out.addAll(mesh);
        return out;
    }

    // Build a String from raw bytes without StringUtil (utf8ArrayToString
    // faults on a ByteArray here). Node names are ASCII; keep it simple and
    // drop non-printable/truncated garbage.
    function bytesToString(b) {
        var s = "";
        for (var i = 0; i < b.size(); i++) {
            var c = b[i] & 0xff;
            if (c == 0) {
                break;
            }
            if (c >= 32 && c < 127) {
                s += c.toChar().toString();
            }
        }
        if (s.length() > 0) {
            return s;
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
                var subEnd = pos + len;
                if (subEnd > b.size()) { subEnd = b.size(); } // salvage partial
                var sub = b.slice(pos, subEnd);
                pos = subEnd;
                if (field == 3) {
                    parseMyInfo(sub, store);
                    result = :myinfo;
                } else if (field == 4) {
                    parseNodeInfo(sub, store);
                    result = :node;
                } else if (field == 1) {
                    store.packetCount++;
                    result = parseMeshPacket(sub, store) ? :message : :packet;
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

    // Returns true when the packet carried a text message we stored.
    // MeshPacket: 1=from(fx32) 4=decoded(Data); Data: 1=portnum 2=payload
    function parseMeshPacket(b, store) {
        var pos = 0;
        var from = null;
        var portnum = 0;
        var payload = null;
        while (pos < b.size()) {
            var r = readVarint(b, pos);
            var tag = r[0].toNumber();
            pos = r[1];
            var field = tag >> 3;
            var wire = tag & 0x7;
            if (wire == 0) {
                r = readVarint(b, pos);
                pos = r[1];
                if (field == 1) { // defensive: some builds varint-encode from
                    from = r[0];
                }
            } else if (wire == 2) {
                r = readVarint(b, pos);
                var len = r[0].toNumber();
                pos = r[1];
                var subEnd = pos + len;
                if (subEnd > b.size()) { subEnd = b.size(); } // salvage partial
                var sub = b.slice(pos, subEnd);
                pos = subEnd;
                if (field == 4) {
                    var dpos = 0;
                    while (dpos < sub.size()) {
                        var dr = readVarint(sub, dpos);
                        var dtag = dr[0].toNumber();
                        dpos = dr[1];
                        var dfield = dtag >> 3;
                        var dwire = dtag & 0x7;
                        if (dwire == 0) {
                            dr = readVarint(sub, dpos);
                            dpos = dr[1];
                            if (dfield == 1) {
                                portnum = dr[0].toNumber();
                            }
                        } else if (dwire == 2) {
                            dr = readVarint(sub, dpos);
                            var dlen = dr[0].toNumber();
                            dpos = dr[1];
                            if (dpos + dlen > sub.size()) { break; }
                            if (dfield == 2) {
                                payload = sub.slice(dpos, dpos + dlen);
                            }
                            dpos += dlen;
                        } else if (dwire == 5) {
                            dpos += 4;
                        } else if (dwire == 1) {
                            dpos += 8;
                        } else {
                            break;
                        }
                    }
                }
            } else if (wire == 5) {
                if (pos + 4 > b.size()) { break; }
                if (field == 1) {
                    from = b.decodeNumber(Lang.NUMBER_FORMAT_UINT32,
                        {:offset => pos, :endianness => Lang.ENDIAN_LITTLE});
                }
                pos += 4;
            } else if (wire == 1) {
                pos += 8;
            } else {
                break;
            }
        }
        if (portnum == 1 && payload != null) {
            store.addMessage(from, bytesToString(payload), false);
            return true;
        }
        return false;
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
                var subEnd = pos + len;
                if (subEnd > b.size()) { subEnd = b.size(); } // salvage partial
                var sub = b.slice(pos, subEnd);
                pos = subEnd;
                if (field == 2) {
                    parseUser(sub, info);
                } else if (field == 6) {
                    parseDeviceMetrics(sub, info);
                }
            } else if (wire == 5) {
                if (pos + 4 > b.size()) { break; }
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
                var subEnd = pos + len;
                if (subEnd > b.size()) { subEnd = b.size(); } // salvage partial
                var sub = b.slice(pos, subEnd);
                pos = subEnd;
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
