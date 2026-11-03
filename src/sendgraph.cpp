// Sendgraph: finds possible message-passing deadlocks in a QNX 8.0 kernel trace, including ones that
// did not happen during the trace. Input is the text traceprinter prints for a tracelogger capture.
//
// QNX services talk with blocking MsgSend. A thread that serves channel A and blocks sending to channel B
// cannot receive on A until B answers. So "A waits for B" is an edge, and a cycle of those edges is a
// deadlock waiting for the right timing, the same idea lockdep applies to locks.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ThreadId {
    int pid = 0, tid = 0;
    bool operator<(const ThreadId &o) const { return pid != o.pid ? pid < o.pid : tid < o.tid; }
    bool operator==(const ThreadId &o) const { return pid == o.pid && tid == o.tid; }
};

struct ChannelId {
    int pid = 0;
    long chid = 0;
    bool operator<(const ChannelId &o) const { return pid != o.pid ? pid < o.pid : chid < o.chid; }
    bool operator==(const ChannelId &o) const { return pid == o.pid && chid == o.chid; }
};

enum EdgeKind { kLoop = 1, kHandler = 2, kDeferred = 4 };

struct Edge {
    ChannelId from, to;
    int kinds = 0;
    long count = 0;
    uint64_t first_t = 0, max_wait = 0;
    ThreadId sender;
    unsigned msg_type = 0;
};

struct Message {  // one MsgSend, from send to reply
    ThreadId client, server;
    ChannelId channel;
    long coid = -1;
    unsigned type = 0;
    uint64_t sent_t = 0, recv_t = 0;
    bool received = false;
};

struct ThreadState {
    long recv_chid = -1;          // channel of its latest MsgReceive
    long pending_coid = -1;       // coid of the MsgSend it is entering
    unsigned pending_type = 0;
    bool in_send = false;
    std::vector<std::pair<uint64_t, Message>> sends;  // resolved sends, for deferred replies
    std::map<long, int> held;     // channel -> unreplied messages this thread took from it
};

struct Analysis {
    std::map<int, std::string> proc_names;
    std::map<ThreadId, std::string> thread_names;
    std::map<ChannelId, std::set<ThreadId>> receivers;
    std::map<std::pair<ChannelId, ChannelId>, Edge> edges;
    std::map<std::pair<int, int>, long> pulse_edges;     // (sender pid, destination pid) -> count
    std::map<ThreadId, ThreadState> threads;
    std::map<std::pair<int, long>, Message> in_flight;   // (server pid, rcvid) -> message
    std::map<std::pair<int, long>, ChannelId> connections;  // (client pid, coid) -> channel
    std::map<ThreadId, Message> blocked;                 // sends with no reply yet, per client thread
    std::vector<Message> unresolved;                     // received before we knew the channel
    uint64_t cycles_per_sec = 0, last_t = 0;
    long events = 0, messages = 0;
    int next_unknown_chid = -1;
};

unsigned long long hex_or_dec(const std::string &s) { return std::strtoull(s.c_str(), nullptr, 0); }

// Finds "key:value" inside a line; the value ends at a space.
bool field(const std::string &line, const char *key, std::string &out) {
    std::string k = std::string(key) + ":";
    size_t at = 0;
    while ((at = line.find(k, at)) != std::string::npos) {
        if (at == 0 || line[at - 1] == ' ') {
            size_t b = at + k.size(), e = line.find(' ', b);
            out = line.substr(b, e == std::string::npos ? std::string::npos : e - b);
            return true;
        }
        at += k.size();
    }
    return false;
}

struct Event {
    uint64_t t = 0;
    int cpu = 0;
    std::string cls, name, rest;
    std::vector<std::string> extra;
    bool get(const char *key, std::string &out) const {
        if (field(rest, key, out)) return true;
        for (const auto &l : extra)
            if (field(l, key, out)) return true;
        return false;
    }
    long num(const char *key, long fallback = -1) const {
        std::string v;
        return get(key, v) ? (long)hex_or_dec(v) : fallback;
    }
};

bool parse_head(const std::string &line, Event &e) {
    // t:0xe85f3c370 CPU:00 COMM            :SND_MESSAGE        rcvid:0x00000001 pid:618518
    if (line.compare(0, 2, "t:") != 0) return false;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) return false;
    e.t = hex_or_dec(line.substr(2, sp - 2));
    size_t cpu = line.find("CPU:", sp);
    if (cpu == std::string::npos) return false;
    e.cpu = std::atoi(line.c_str() + cpu + 4);
    size_t b = line.find(' ', cpu);
    if (b == std::string::npos) return false;
    b = line.find_first_not_of(' ', b);
    size_t colon = line.find(':', b);
    if (b == std::string::npos || colon == std::string::npos) return false;
    e.cls = line.substr(b, line.find_last_not_of(' ', colon - 1) - b + 1);
    size_t ne = line.find(' ', colon);
    e.name = line.substr(colon + 1, ne == std::string::npos ? std::string::npos : ne - colon - 1);
    size_t slash = e.name.find('/');
    if (slash != std::string::npos) e.name.resize(slash);
    e.rest = ne == std::string::npos ? "" : line.substr(ne);
    size_t first = e.rest.find_first_not_of(' ');
    e.rest = first == std::string::npos ? "" : e.rest.substr(first);
    return true;
}

class Parser {
  public:
    explicit Parser(Analysis &a) : a_(a) {}

    void feed(const Event &e) {
        a_.events++;
        a_.last_t = std::max(a_.last_t, e.t);
        if (e.cls == "THREAD") return thread_event(e);
        if (e.cls == "PROCESS") return process_event(e);
        if (e.cls == "KER_CALL") return kernel_call(e);
        if (e.cls == "COMM") return comm_event(e);
    }

    // A thread already blocked in MsgReceive when the trace began never shows which channel it serves
    // until its next receive, so those messages wait here and are placed once it does.
    void finish() {
        for (auto &m : a_.unresolved) {
            long chid = a_.threads[m.server].recv_chid;
            if (chid < 0) chid = a_.next_unknown_chid;
            place(m, ChannelId{m.server.pid, chid});
        }
        a_.unresolved.clear();
        for (auto &kv : a_.in_flight) {  // still queued or being handled when the trace ended
            Message &m = kv.second;
            if (!m.received) {
                auto c = a_.connections.find({m.client.pid, m.coid});
                m.channel = c != a_.connections.end() ? c->second : only_channel(m.server.pid);
                add_send_edges(m, a_.last_t - m.sent_t);
                a_.receivers[m.channel];
            }
        }
    }

  private:
    Analysis &a_;
    std::map<int, ThreadId> running_;

    // The channel a queued message must be for, if its process has just one. Otherwise it stays unknown.
    ChannelId only_channel(int pid) const {
        ChannelId found{pid, a_.next_unknown_chid};
        int n = 0;
        for (const auto &kv : a_.receivers)
            if (kv.first.pid == pid && !kv.second.empty() && n++ == 0) found = kv.first;
        return n == 1 ? found : ChannelId{pid, a_.next_unknown_chid};
    }

    ThreadId current(int cpu) const {
        auto it = running_.find(cpu);
        return it == running_.end() ? ThreadId{} : it->second;
    }

    void thread_event(const Event &e) {
        std::string pid, tid;
        if (!field(e.rest, "pid", pid) || !field(e.rest, "tid", tid)) return;
        ThreadId id{std::atoi(pid.c_str()), std::atoi(tid.c_str())};
        if (e.name == "THRUNNING") running_[e.cpu] = id;
    }

    void process_event(const Event &e) {
        std::string pid, tid, name;
        if (!e.get("name", name)) return;
        if (!e.get("pid", pid)) return;
        if (e.name == "PROCCREATE_NAME") {
            size_t slash = name.rfind('/');
            a_.proc_names[std::atoi(pid.c_str())] = slash == std::string::npos ? name : name.substr(slash + 1);
        } else if (e.name == "PROCTHREAD_NAME" && e.get("tid", tid)) {
            a_.thread_names[ThreadId{std::atoi(pid.c_str()), std::atoi(tid.c_str())}] = name;
        }
    }

    void kernel_call(const Event &e) {
        ThreadId me = current(e.cpu);
        ThreadState &t = a_.threads[me];
        if (e.name == "MSG_RECEIVEV") {
            t.recv_chid = e.num("chid");
        } else if (e.name == "MSG_SENDV" || e.name == "MSG_SENDVNC") {
            t.pending_coid = e.num("coid");
            t.pending_type = 0;
            for (const auto &l : e.extra) {  // msg[0]:"" (0x00000200 0x000003e9 0x00000000)
                size_t p = l.find("(0x");
                if (l.find("msg[0]") != std::string::npos && p != std::string::npos) {
                    t.pending_type = (unsigned)(std::strtoul(l.c_str() + p + 1, nullptr, 0) & 0xffff);
                }
            }
        } else if (e.name == "MSG_REPLYV" || e.name == "MSG_ERROR") {
            finish_message(me, e.num("rcvid"), e.t);
        }
    }

    void comm_event(const Event &e) {
        ThreadId me = current(e.cpu);
        if (e.name == "SND_MESSAGE") {
            ThreadState &t = a_.threads[me];
            Message m;
            m.client = me;
            m.server.pid = (int)e.num("pid");
            m.coid = t.pending_coid;
            m.type = t.pending_type;
            m.sent_t = e.t;
            a_.in_flight[{m.server.pid, e.num("rcvid")}] = m;
            a_.messages++;
        } else if (e.name == "REC_MESSAGE") {
            auto it = a_.in_flight.find({me.pid, e.num("rcvid")});
            if (it == a_.in_flight.end()) return;  // the send came before the trace
            Message &m = it->second;
            m.server = me;
            m.recv_t = e.t;
            m.received = true;
            long chid = a_.threads[me].recv_chid;
            if (chid >= 0) {
                place(m, ChannelId{me.pid, chid});
            } else {
                a_.unresolved.push_back(m);
            }
        } else if (e.name == "SND_PULSE" || e.name == "SND_PULSE_EXE") {
            if (me.pid && me.pid != e.num("pid")) a_.pulse_edges[{me.pid, (int)e.num("pid")}]++;
        }
    }

    void place(Message &m, ChannelId channel) {
        m.channel = channel;
        a_.receivers[channel].insert(m.server);
        if (m.coid >= 0) a_.connections[{m.client.pid, m.coid}] = channel;
        a_.threads[m.server].held[channel.chid]++;
        add_send_edges(m, m.recv_t >= m.sent_t ? m.recv_t - m.sent_t : 0);
        a_.threads[m.client].sends.push_back({m.sent_t, m});
        // Keep the channel on the live record too, for the reply.
        for (auto &kv : a_.in_flight)
            if (kv.first.first == m.server.pid && kv.second.sent_t == m.sent_t && kv.second.client == m.client)
                kv.second.channel = channel;
    }

    Edge &edge(ChannelId from, ChannelId to) {
        Edge &e = a_.edges[{from, to}];
        e.from = from;
        e.to = to;
        return e;
    }

    // Every channel the sender serves now waits for the destination.
    void add_send_edges(const Message &m, uint64_t wait) {
        const ThreadState &t = a_.threads[m.client];
        for (auto &kv : a_.receivers) {
            if (kv.first.pid != m.client.pid || !kv.second.count(m.client)) continue;
            Edge &e = edge(kv.first, m.channel);
            auto held = t.held.find(kv.first.chid);
            e.kinds |= held != t.held.end() && held->second > 0 ? kHandler : kLoop;
            if (!e.count++) {
                e.first_t = m.sent_t;
                e.sender = m.client;
                e.msg_type = m.type;
            }
            e.max_wait = std::max(e.max_wait, wait);
        }
        pending_edges_.push_back(m);
    }

    // A thread can begin serving a channel after it has already sent, so sends are replayed at the end.
    std::vector<Message> pending_edges_;

    void finish_message(ThreadId replier, long rcvid, uint64_t t) {
        auto it = a_.in_flight.find({replier.pid, rcvid});
        if (it == a_.in_flight.end()) return;
        Message m = it->second;
        a_.in_flight.erase(it);
        if (!m.received) return;
        auto &held = a_.threads[m.server].held;
        auto h = held.find(m.channel.chid);
        if (h != held.end() && h->second > 0) h->second--;
        if (t > m.sent_t) {
            for (auto &kv : a_.edges)
                if (kv.second.to == m.channel && kv.second.sender == m.client)
                    kv.second.max_wait = std::max(kv.second.max_wait, t - m.sent_t);
        }
        if (replier == m.server) return;
        // Deferred reply: the thread that answered was doing other sends on this client's behalf.
        const ThreadState &w = a_.threads[replier];
        for (auto s = w.sends.rbegin(); s != w.sends.rend() && s->first >= m.recv_t; ++s) {
            Edge &e = edge(m.channel, s->second.channel);
            e.kinds |= kDeferred;
            if (!e.count++) {
                e.first_t = s->first;
                e.sender = replier;
                e.msg_type = s->second.type;
            }
        }
    }

  public:
    // Second look at all sends now that every thread's channels are known.
    void replay_sends() {
        std::vector<Message> sends;
        sends.swap(pending_edges_);
        for (const Message &m : sends) {
            for (auto &kv : a_.receivers) {
                if (kv.first.pid != m.client.pid || !kv.second.count(m.client)) continue;
                auto it = a_.edges.find({kv.first, m.channel});
                if (it != a_.edges.end()) continue;
                Edge &e = edge(kv.first, m.channel);
                e.kinds |= kLoop;
                e.count++;
                e.first_t = m.sent_t;
                e.sender = m.client;
                e.msg_type = m.type;
            }
        }
    }
};

std::string proc_name(const Analysis &a, int pid) {
    auto it = a.proc_names.find(pid);
    return it == a.proc_names.end() ? "pid" + std::to_string(pid) : it->second;
}

std::string channel_name(const Analysis &a, const ChannelId &c) {
    std::string s = proc_name(a, c.pid);
    int same = 0;
    for (const auto &kv : a.receivers)
        if (kv.first.pid == c.pid) same++;
    if (same > 1) s += c.chid < 0 ? "#?" : "#" + std::to_string(c.chid);
    return s;
}

std::string thread_name(const Analysis &a, const ThreadId &t) {
    auto it = a.thread_names.find(t);
    std::string s = proc_name(a, t.pid) + "/" + std::to_string(t.tid);
    if (it != a.thread_names.end()) s += " (" + it->second + ")";
    return s;
}

double micros(const Analysis &a, uint64_t cycles) { return a.cycles_per_sec ? 1e6 * (double)cycles / (double)a.cycles_per_sec : 0; }

// Names of the common QNX resource-manager messages, so the report reads like the code that sent them.
const char *message_name(unsigned type) {
    switch (type) {
        case 0x100: return "_IO_CONNECT";
        case 0x101: return "_IO_READ";
        case 0x102: return "_IO_WRITE";
        case 0x103: return "_IO_RSVD_CLOSE_OCB";
        case 0x104: return "_IO_STAT";
        case 0x105: return "_IO_NOTIFY";
        case 0x106: return "_IO_DEVCTL";
        case 0x107: return "_IO_RSVD_UNBLOCK";
        case 0x108: return "_IO_PATHCONF";
        case 0x109: return "_IO_LSEEK";
        case 0x10a: return "_IO_CHMOD";
        case 0x10b: return "_IO_CHOWN";
        case 0x10c: return "_IO_UTIME";
        case 0x10d: return "_IO_OPENFD";
        case 0x10e: return "_IO_FDINFO";
        case 0x10f: return "_IO_LOCK";
        case 0x110: return "_IO_SPACE";
        case 0x111: return "_IO_SHUTDOWN";
        case 0x112: return "_IO_MMAP";
        case 0x113: return "_IO_MSG";
        case 0x115: return "_IO_DUP";
        case 0x116: return "_IO_CLOSE";
        case 0x119: return "_IO_SYNC";
        default: return nullptr;
    }
}

std::string type_label(unsigned type) {
    const char *n = message_name(type);
    if (n) return n;
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", type);
    return buf;
}

struct Cycle {
    std::vector<ChannelId> path;  // path[i] waits for path[i+1], and the last for the first
    size_t capacity = 0;          // threads that must all be stuck: the smallest pool on the cycle
};

// Strongly connected components (Tarjan), then one shortest cycle through each component.
std::vector<Cycle> find_cycles(const Analysis &a, std::vector<std::vector<ChannelId>> *order_out) {
    std::map<ChannelId, std::vector<ChannelId>> next;
    std::set<ChannelId> nodes;
    for (const auto &kv : a.edges) {
        next[kv.first.first].push_back(kv.first.second);
        nodes.insert(kv.first.first);
        nodes.insert(kv.first.second);
    }
    std::map<ChannelId, int> index, low;
    std::set<ChannelId> on_stack;
    std::vector<ChannelId> stack;
    std::vector<std::vector<ChannelId>> comps;
    int counter = 0;
    // Written with an explicit stack, because a whole system's graph can be deep.
    for (const ChannelId &root : nodes) {
        if (index.count(root)) continue;
        struct Frame { ChannelId v; size_t i; };
        std::vector<Frame> call{{root, 0}};
        index[root] = low[root] = counter++;
        stack.push_back(root);
        on_stack.insert(root);
        while (!call.empty()) {
            Frame &f = call.back();
            const auto &succ = next[f.v];
            if (f.i < succ.size()) {
                ChannelId w = succ[f.i++];
                if (!index.count(w)) {
                    index[w] = low[w] = counter++;
                    stack.push_back(w);
                    on_stack.insert(w);
                    call.push_back({w, 0});
                } else if (on_stack.count(w)) {
                    low[f.v] = std::min(low[f.v], index[w]);
                }
            } else {
                ChannelId v = f.v;
                if (low[v] == index[v]) {
                    std::vector<ChannelId> comp;
                    for (;;) {
                        ChannelId w = stack.back();
                        stack.pop_back();
                        on_stack.erase(w);
                        comp.push_back(w);
                        if (w == v) break;
                    }
                    comps.push_back(comp);
                }
                call.pop_back();
                if (!call.empty()) low[call.back().v] = std::min(low[call.back().v], low[v]);
            }
        }
    }
    if (order_out) *order_out = comps;  // leaves first: Tarjan finishes a component after all it reaches

    std::vector<Cycle> out;
    for (const auto &comp : comps) {
        bool self = comp.size() == 1 && a.edges.count({comp[0], comp[0]});
        if (comp.size() < 2 && !self) continue;
        std::set<ChannelId> in(comp.begin(), comp.end());
        std::vector<ChannelId> best;
        // Shortest cycle through each node of the component, by breadth-first search inside it.
        for (const ChannelId &start : comp) {
            std::map<ChannelId, ChannelId> parent;
            std::vector<ChannelId> queue{start};
            bool found = false;
            ChannelId last;
            for (size_t q = 0; q < queue.size() && !found; q++) {
                for (const ChannelId &w : next[queue[q]]) {
                    if (!in.count(w)) continue;
                    if (w == start) {
                        found = true;
                        last = queue[q];
                        break;
                    }
                    if (!parent.count(w) && !(w == start)) {
                        parent[w] = queue[q];
                        queue.push_back(w);
                    }
                }
            }
            if (!found) continue;
            std::vector<ChannelId> path{last};
            while (!(path.back() == start)) path.push_back(parent[path.back()]);
            std::reverse(path.begin(), path.end());
            if (best.empty() || path.size() < best.size()) best = path;
        }
        if (best.empty()) continue;
        Cycle c;
        c.path = best;
        c.capacity = SIZE_MAX;
        for (const ChannelId &n : best) {
            auto r = a.receivers.find(n);
            c.capacity = std::min(c.capacity, r == a.receivers.end() || r->second.empty() ? (size_t)1 : r->second.size());
        }
        out.push_back(c);
    }
    return out;
}

// The send hierarchy this system already follows, lowest level first. Levels are the longest chain of
// waits below a channel; a component in a cycle shares one level, marked by the cycle report.
std::map<ChannelId, int> levels(const Analysis &a, const std::vector<std::vector<ChannelId>> &order) {
    std::map<ChannelId, int> comp_of, level;
    for (size_t i = 0; i < order.size(); i++)
        for (const auto &c : order[i]) comp_of[c] = (int)i;
    std::vector<int> comp_level(order.size(), 0);
    for (size_t i = 0; i < order.size(); i++) {
        for (const auto &kv : a.edges) {
            if (comp_of[kv.first.first] != (int)i) continue;
            int to = comp_of[kv.first.second];
            if (to != (int)i) comp_level[i] = std::max(comp_level[i], comp_level[to] + 1);
        }
        for (const auto &c : order[i]) level[c] = comp_level[i];
    }
    return level;
}

std::string json_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c < 0x20) continue;
        o += c;
    }
    return o;
}

std::string html_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

void print_edge_evidence(const Analysis &a, const Edge &e, std::ostream &out) {
    out << "    " << channel_name(a, e.from) << " waits for " << channel_name(a, e.to) << "\n";
    out << "      " << thread_name(a, e.sender) << " sent " << type_label(e.msg_type)
        << ((e.kinds & kHandler) ? " while handling a message"
            : (e.kinds & kDeferred) ? " before answering a message it was handed"
                                    : " from its receive loop")
        << ", seen " << e.count << " time" << (e.count == 1 ? "" : "s");
    char buf[64];
    std::snprintf(buf, sizeof buf, ", longest wait %.0f us", micros(a, e.max_wait));
    if (a.cycles_per_sec) out << buf;
    out << "\n";
}

void print_report(const Analysis &a, const std::vector<Cycle> &cycles, const std::map<ChannelId, int> &level, std::ostream &out) {
    out << "Sendgraph: " << a.messages << " messages between " << a.receivers.size() << " channels, "
        << a.edges.size() << " wait edges.\n\n";
    if (cycles.empty()) {
        out << "No wait cycles. No deadlock through message passing on the paths this trace exercised.\n";
    } else {
        out << "POSSIBLE DEADLOCK: " << cycles.size() << " wait cycle" << (cycles.size() == 1 ? "" : "s")
            << ". The trace does not have to contain the hang for this to be true.\n";
        int n = 0;
        for (const Cycle &c : cycles) {
            out << "\nCycle " << ++n << ": ";
            for (const ChannelId &ch : c.path) out << channel_name(a, ch) << " -> ";
            out << channel_name(a, c.path[0]) << "\n";
            if (c.capacity <= 1) {
                out << "  A single thread serves one of these channels, so one unlucky interleaving hangs them all.\n";
            } else {
                out << "  Every channel here has a thread pool, the smallest of " << c.capacity
                    << " threads. It hangs when " << c.capacity << " requests are in this cycle at once.\n";
            }
            for (size_t i = 0; i < c.path.size(); i++) {
                auto e = a.edges.find({c.path[i], c.path[(i + 1) % c.path.size()]});
                if (e != a.edges.end()) print_edge_evidence(a, e->second, out);
            }
        }
    }
    std::map<std::string, long> pulse_lines;
    for (const auto &kv : a.pulse_edges) {
        if (kv.first.first == 1 || kv.first.second == 1) continue;  // the kernel's own timers and events
        pulse_lines[proc_name(a, kv.first.first) + " -> " + proc_name(a, kv.first.second)] += kv.second;
    }
    if (!pulse_lines.empty()) {
        out << "\nPulses, which never block and so cannot deadlock:\n";
        for (const auto &kv : pulse_lines) out << "  " << kv.first << " (" << kv.second << ")\n";
    }
    if (!level.empty()) {
        int top = 0;
        for (const auto &kv : level) top = std::max(top, kv.second);
        out << "\nSend hierarchy seen in this trace (a thread should only send to a lower level):\n";
        for (int l = top; l >= 0; l--) {
            std::string names;
            for (const auto &kv : level)
                if (kv.second == l) names += (names.empty() ? "" : ", ") + channel_name(a, kv.first);
            out << "  level " << l << ": " << names << "\n";
        }
    }
}

void write_dot(const Analysis &a, const std::vector<Cycle> &cycles, std::ostream &out) {
    std::set<std::pair<ChannelId, ChannelId>> in_cycle;
    for (const Cycle &c : cycles)
        for (size_t i = 0; i < c.path.size(); i++) in_cycle.insert({c.path[i], c.path[(i + 1) % c.path.size()]});
    out << "digraph sendgraph {\n  rankdir=TB;\n  node [shape=box, fontname=\"monospace\"];\n";
    for (const auto &kv : a.edges) {
        bool hot = in_cycle.count(kv.first) > 0;
        out << "  \"" << json_escape(channel_name(a, kv.first.first)) << "\" -> \"" << json_escape(channel_name(a, kv.first.second))
            << "\" [label=\"" << kv.second.count << "\"" << (hot ? ", color=red, penwidth=2" : "") << "];\n";
    }
    out << "}\n";
}

void write_json(const Analysis &a, const std::vector<Cycle> &cycles, std::ostream &out) {
    out << "{\n  \"messages\": " << a.messages << ",\n  \"edges\": [\n";
    bool first = true;
    for (const auto &kv : a.edges) {
        const Edge &e = kv.second;
        out << (first ? "" : ",\n") << "    {\"from\": \"" << json_escape(channel_name(a, e.from)) << "\", \"to\": \""
            << json_escape(channel_name(a, e.to)) << "\", \"count\": " << e.count << ", \"message\": \"" << type_label(e.msg_type)
            << "\", \"max_wait_us\": " << (long)micros(a, e.max_wait) << ", \"sender\": \"" << json_escape(thread_name(a, e.sender)) << "\"}";
        first = false;
    }
    out << "\n  ],\n  \"cycles\": [\n";
    first = true;
    for (const Cycle &c : cycles) {
        out << (first ? "" : ",\n") << "    {\"threads_needed\": " << c.capacity << ", \"path\": [";
        for (size_t i = 0; i < c.path.size(); i++) out << (i ? ", " : "") << "\"" << json_escape(channel_name(a, c.path[i])) << "\"";
        out << "]}";
        first = false;
    }
    out << "\n  ]\n}\n";
}

void write_html(const Analysis &a, const std::vector<Cycle> &cycles, const std::map<ChannelId, int> &level, const std::string &title, std::ostream &out) {
    std::set<std::pair<ChannelId, ChannelId>> in_cycle;
    std::set<ChannelId> hot;
    for (const Cycle &c : cycles)
        for (size_t i = 0; i < c.path.size(); i++) {
            in_cycle.insert({c.path[i], c.path[(i + 1) % c.path.size()]});
            hot.insert(c.path[i]);
        }
    // Layers by send-hierarchy level, top level on top.
    std::map<int, std::vector<ChannelId>> rows;
    int top = 0;
    for (const auto &kv : level) {
        rows[kv.second].push_back(kv.first);
        top = std::max(top, kv.second);
    }
    size_t widest = 1;
    for (const auto &r : rows) widest = std::max(widest, r.second.size());
    const int W = 190, H = 110;
    int width = (int)widest * W + 40, height = (top + 1) * H + 30;
    std::map<ChannelId, std::pair<int, int>> pos;
    for (auto &r : rows) {
        int y = (top - r.first) * H + 20;
        int gap = width / ((int)r.second.size() + 1);
        for (size_t i = 0; i < r.second.size(); i++) pos[r.second[i]] = {gap * ((int)i + 1), y};
    }
    out << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>"
        << html_escape(title) << "</title><style>"
           ":root{--bg:#f6f7f8;--card:#fff;--ink:#1c2228;--muted:#5d6a75;--line:#d5dbe0;--hot:#b3261e;--hot-bg:#fbe9e7;--ok:#1b6e4b;--ok-bg:#e1f2ea;--node:#eef1f4}"
           "@media(prefers-color-scheme:dark){:root{--bg:#14181c;--card:#1c2228;--ink:#e6eaee;--muted:#9aa7b2;--line:#323b44;--hot:#f2877e;--hot-bg:#3a1f1c;--ok:#6fd2a4;--ok-bg:#16322a;--node:#262e36}}"
           "body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 system-ui,sans-serif}main{max-width:1000px;margin:0 auto;padding:24px 16px}"
           "h1{font-size:1.6rem;margin:0 0 4px}h2{font-size:1.1rem;margin:0 0 10px}.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:16px;margin-top:14px}"
           ".small{font-size:.875rem;color:var(--muted)}.verdict{font-weight:650;padding:12px 16px;border-radius:8px;margin-top:14px}.bad{background:var(--hot-bg);color:var(--hot)}.good{background:var(--ok-bg);color:var(--ok)}"
           ".mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.86em}.scroll{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:.9rem}th,td{text-align:left;padding:5px 12px 5px 0;border-bottom:1px solid var(--line);white-space:nowrap}th{color:var(--muted);font-weight:600}"
           "svg text{font:13px ui-monospace,Menlo,Consolas,monospace;fill:var(--ink)}</style></head><body><main><h1>"
        << html_escape(title) << "</h1><p class=\"small\">" << a.messages << " messages, " << a.receivers.size() << " channels, " << a.edges.size() << " wait edges.</p>";
    if (cycles.empty()) {
        out << "<div class=\"verdict good\">No wait cycles on the paths this trace exercised.</div>";
    } else {
        out << "<div class=\"verdict bad\">Possible deadlock: " << cycles.size() << " wait cycle" << (cycles.size() == 1 ? "" : "s") << ".</div>";
    }
    out << "<section class=\"card\"><h2>Who waits for whom</h2><div class=\"scroll\" tabindex=\"0\" role=\"region\" aria-label=\"Wait graph\"><svg width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\" role=\"img\" aria-label=\"Wait graph of " << a.receivers.size() << " channels, cycles in red\">"
        << "<defs><marker id=\"a\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"7\" markerHeight=\"7\" orient=\"auto-start-reverse\"><path d=\"M0 0L10 5L0 10z\" fill=\"var(--muted)\"/></marker>"
        << "<marker id=\"h\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"7\" markerHeight=\"7\" orient=\"auto-start-reverse\"><path d=\"M0 0L10 5L0 10z\" fill=\"var(--hot)\"/></marker></defs>";
    for (const auto &kv : a.edges) {
        auto f = pos.find(kv.first.first), t = pos.find(kv.first.second);
        if (f == pos.end() || t == pos.end()) continue;
        bool isHot = in_cycle.count(kv.first) > 0;
        int x1 = f->second.first, y1 = f->second.second + 40, x2 = t->second.first, y2 = t->second.second;
        if (y2 <= y1 - 40) { y1 = f->second.second; y2 = t->second.second + 40; }  // arrow going up
        int mx = (x1 + x2) / 2 + (y2 < y1 ? 40 : 0), my = (y1 + y2) / 2;
        out << "<path d=\"M" << x1 << " " << y1 << " Q" << mx << " " << my << " " << x2 << " " << y2 << "\" fill=\"none\" stroke=\"" << (isHot ? "var(--hot)" : "var(--muted)")
            << "\" stroke-width=\"" << (isHot ? 2.5 : 1.2) << "\" marker-end=\"url(#" << (isHot ? "h" : "a") << ")\"><title>" << html_escape(channel_name(a, kv.first.first)) << " waits for " << html_escape(channel_name(a, kv.first.second)) << "</title></path>";
    }
    for (const auto &kv : pos) {
        bool isHot = hot.count(kv.first) > 0;
        std::string name = channel_name(a, kv.first);
        size_t threads = 0;
        auto r = a.receivers.find(kv.first);
        if (r != a.receivers.end()) threads = r->second.size();
        out << "<rect x=\"" << kv.second.first - 80 << "\" y=\"" << kv.second.second << "\" width=\"160\" height=\"40\" rx=\"5\" fill=\"" << (isHot ? "var(--hot-bg)" : "var(--node)") << "\" stroke=\"" << (isHot ? "var(--hot)" : "var(--line)")
            << "\"/><text x=\"" << kv.second.first << "\" y=\"" << kv.second.second + 17 << "\" text-anchor=\"middle\">" << html_escape(name.substr(0, 20))
            << "</text><text x=\"" << kv.second.first << "\" y=\"" << kv.second.second + 33 << "\" text-anchor=\"middle\" style=\"fill:var(--muted);font-size:11px\">" << threads << " thread" << (threads == 1 ? "" : "s") << "</text>";
    }
    out << "</svg></div></section>";
    int n = 0;
    for (const Cycle &c : cycles) {
        out << "<section class=\"card\"><h2>Cycle " << ++n << "</h2><p class=\"mono\">";
        for (const ChannelId &ch : c.path) out << html_escape(channel_name(a, ch)) << " &rarr; ";
        out << html_escape(channel_name(a, c.path[0])) << "</p><p>"
            << (c.capacity <= 1 ? "A single thread serves one of these channels, so one unlucky interleaving hangs them all."
                                : "It hangs when " + std::to_string(c.capacity) + " requests are in this cycle at once.")
            << "</p></section>";
    }
    out << "<section class=\"card\"><h2>Evidence</h2><div class=\"scroll\" tabindex=\"0\" role=\"region\" aria-label=\"Edges\"><table><thead><tr><th scope=\"col\">Channel</th><th scope=\"col\">Waits for</th><th scope=\"col\">Sender</th><th scope=\"col\">Message</th><th scope=\"col\">Seen</th><th scope=\"col\">Longest wait</th></tr></thead><tbody>";
    for (const auto &kv : a.edges) {
        const Edge &e = kv.second;
        char wait[32];
        std::snprintf(wait, sizeof wait, "%.0f us", micros(a, e.max_wait));
        out << "<tr><td class=\"mono\">" << html_escape(channel_name(a, e.from)) << "</td><td class=\"mono\">" << html_escape(channel_name(a, e.to)) << "</td><td class=\"mono\">" << html_escape(thread_name(a, e.sender)) << "</td><td class=\"mono\">" << type_label(e.msg_type) << "</td><td>" << e.count << "</td><td>" << wait << "</td></tr>";
    }
    out << "</tbody></table></div></section></main></body></html>\n";
}

int usage() {
    std::cerr << "usage: traceprinter -f trace.kev | sendgraph [options]\n"
                 "       sendgraph [options] trace.txt\n"
                 "  --dot FILE    write the wait graph as Graphviz\n"
                 "  --json FILE   write the result as JSON\n"
                 "  --html FILE   write a self-contained report\n"
                 "  --quiet       no text report; use the exit code\n"
                 "Exit code: 0 no cycle, 1 at least one cycle, 2 the input is not a usable trace.\n";
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    std::string input, dot, json, html;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dot" && i + 1 < argc) dot = argv[++i];
        else if (arg == "--json" && i + 1 < argc) json = argv[++i];
        else if (arg == "--html" && i + 1 < argc) html = argv[++i];
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--help" || arg == "-h") return usage();
        else if (!arg.empty() && arg[0] == '-' && arg != "-") return usage();
        else input = arg;
    }
    std::ifstream file;
    if (!input.empty() && input != "-") {
        file.open(input);
        if (!file) {
            std::cerr << "sendgraph: cannot read " << input << "\n";
            return 2;
        }
    }
    std::istream &in = file.is_open() ? file : std::cin;

    Analysis a;
    Parser parser(a);
    Event event;
    bool have = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 2, "t:") == 0) {
            if (have) parser.feed(event);
            event = Event();
            have = parse_head(line, event);
        } else if (have && !line.empty() && line[0] == ' ') {
            size_t first = line.find_first_not_of(' ');
            if (first != std::string::npos) event.extra.push_back(line.substr(first));
        } else if (line.find("TRACE_CYCLES_PER_SEC::") != std::string::npos) {
            a.cycles_per_sec = std::strtoull(line.c_str() + line.find("::") + 2, nullptr, 10);
        }
    }
    if (have) parser.feed(event);
    parser.finish();
    parser.replay_sends();
    if (a.events == 0) {
        std::cerr << "sendgraph: no trace events in the input. Expected the text output of traceprinter.\n";
        return 2;
    }
    if (a.messages == 0) {
        std::cerr << "sendgraph: the trace has no communication events. Capture it with tracelogger -w and without -F 6.\n";
        return 2;
    }

    std::vector<std::vector<ChannelId>> order;
    std::vector<Cycle> cycles = find_cycles(a, &order);
    std::map<ChannelId, int> level = levels(a, order);
    if (!quiet) print_report(a, cycles, level, std::cout);
    if (!dot.empty()) {
        std::ofstream f(dot);
        write_dot(a, cycles, f);
    }
    if (!json.empty()) {
        std::ofstream f(json);
        write_json(a, cycles, f);
    }
    if (!html.empty()) {
        std::ofstream f(html);
        write_html(a, cycles, level, "Sendgraph report", f);
    }
    return cycles.empty() ? 0 : 1;
}
