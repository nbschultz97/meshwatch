using Toybox.Time;

// Holds everything we have learned about the mesh, keyed by node num.
class NodeStore {
    var myNum = null;
    var nodes = {};      // num (Long) -> { :shortName, :longName, :battery, :lastHeard, :snr, :hops }
    var order = [];      // node nums in arrival order, for stable display
    var packetCount = 0; // MeshPackets seen after config sync (proves live traffic)
    var messages = [];   // newest first: { :from, :text, :ts, :out }
    var unread = 0;

    function addMessage(from, text, out) {
        messages = [{:from => from, :text => text,
            :ts => Time.now().value(), :out => out}].addAll(messages);
        if (messages.size() > 30) {
            messages = messages.slice(0, 30);
        }
        if (!out) {
            unread++;
        }
    }

    function nameFor(num) {
        if (num == null) {
            return "????";
        }
        var n = nodes.get(num);
        if (n != null && n.get(:shortName) != null) {
            return n.get(:shortName);
        }
        return "!" + (num.toNumber() & 0xffff).format("%04x");
    }

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
