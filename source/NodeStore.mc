using Toybox.Time;

// Holds everything we have learned about the mesh, keyed by node num.
class NodeStore {
    var myNum = null;
    var nodes = {};      // num (Long) -> { :shortName, :longName, :battery, :lastHeard, :snr, :hops }
    var order = [];      // node nums in arrival order, for stable display
    var packetCount = 0; // MeshPackets seen after config sync (proves live traffic)

    function upsert(num, info) {
        var existing = nodes.get(num);
        if (existing == null) {
            order.add(num);
            nodes.put(num, info);
        } else {
            var keys = info.keys();
            for (var i = 0; i < keys.size(); i++) {
                existing.put(keys[i], info.get(keys[i]));
            }
        }
    }

    function count() {
        return order.size();
    }

    function get(idx) {
        return nodes.get(order[idx]);
    }

    function numAt(idx) {
        return order[idx];
    }
}
