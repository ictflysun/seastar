/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef TCP_HH_
#define TCP_HH_

#include "core/shared_ptr.hh"
#include "core/queue.hh"
#include "core/semaphore.hh"
#include "core/print.hh"
#include "net.hh"
#include "ip_checksum.hh"
#include "ip.hh"
#include "const.hh"
#include "packet-util.hh"
#include <unordered_map>
#include <map>
#include <functional>
#include <deque>
#include <chrono>
#include <experimental/optional>
#include <random>
#include <stdexcept>

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include <cryptopp/md5.h>

using namespace std::chrono_literals;

namespace net {

class tcp_hdr;

class tcp_error : public std::runtime_error {
public:
    tcp_error(const std::string& msg) : std::runtime_error(msg) {}
};

class tcp_reset_error : public tcp_error {
public:
    tcp_reset_error() : tcp_error("connection is reset") {}
};

class tcp_connect_error : public tcp_error {
public:
    tcp_connect_error() : tcp_error("fail to connect") {}
};

class tcp_refused_error : public tcp_error {
public:
    tcp_refused_error() : tcp_error("connection refused") {}
};

enum class tcp_state : uint16_t {
    CLOSED          = (1 << 0),
    LISTEN          = (1 << 1),
    SYN_SENT        = (1 << 2),
    SYN_RECEIVED    = (1 << 3),
    ESTABLISHED     = (1 << 4),
    FIN_WAIT_1      = (1 << 5),
    FIN_WAIT_2      = (1 << 6),
    CLOSE_WAIT      = (1 << 7),
    CLOSING         = (1 << 8),
    LAST_ACK        = (1 << 9),
    TIME_WAIT       = (1 << 10)
};

inline tcp_state operator|(tcp_state s1, tcp_state s2) {
    return tcp_state(uint16_t(s1) | uint16_t(s2));
}

template <typename... Args>
void tcp_debug(const char* fmt, Args&&... args) {
#if TCP_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

struct tcp_option {
    // The kind and len field are fixed and defined in TCP protocol
    enum class option_kind: uint8_t { mss = 2, win_scale = 3, sack = 4, timestamps = 8,  nop = 1, eol = 0 };
    enum class option_len:  uint8_t { mss = 4, win_scale = 3, sack = 2, timestamps = 10, nop = 1, eol = 1 };
    struct mss {
        option_kind kind = option_kind::mss;
        option_len len = option_len::mss;
        packed<uint16_t> mss;
        template <typename Adjuster>
        void adjust_endianness(Adjuster a) { a(mss); }
    } __attribute__((packed));
    struct win_scale {
        option_kind kind = option_kind::win_scale;
        option_len len = option_len::win_scale;
        uint8_t shift;
    } __attribute__((packed));
    struct sack {
        option_kind kind = option_kind::sack;
        option_len len = option_len::sack;
    } __attribute__((packed));
    struct timestamps {
        option_kind kind = option_kind::timestamps;
        option_len len = option_len::timestamps;
        packed<uint32_t> t1;
        packed<uint32_t> t2;
        template <typename Adjuster>
        void adjust_endianness(Adjuster a) { a(t1, t2); }
    } __attribute__((packed));
    struct nop {
        option_kind kind = option_kind::nop;
    } __attribute__((packed));
    struct eol {
        option_kind kind = option_kind::eol;
    } __attribute__((packed));
    static const uint8_t align = 4;

    void parse(uint8_t* beg, uint8_t* end);
    uint8_t fill(tcp_hdr* th, uint8_t option_size);
    uint8_t get_size(bool syn_on, bool ack_on);

    // For option negotiattion
    bool _mss_received = false;
    bool _win_scale_received = false;
    bool _timestamps_received = false;
    bool _sack_received = false;

    // Option data
    uint16_t _remote_mss = 536;
    uint16_t _local_mss;
    uint8_t _remote_win_scale = 0;
    uint8_t _local_win_scale = 0;
};
inline uint8_t*& operator+=(uint8_t*& x, tcp_option::option_len len) { x += uint8_t(len); return x; }
inline uint8_t& operator+=(uint8_t& x, tcp_option::option_len len) { x += uint8_t(len); return x; }

struct tcp_seq {
    uint32_t raw;
};

inline tcp_seq ntoh(tcp_seq s) {
    return tcp_seq { ntoh(s.raw) };
}

inline tcp_seq hton(tcp_seq s) {
    return tcp_seq { hton(s.raw) };
}

inline
std::ostream& operator<<(std::ostream& os, tcp_seq s) {
    return os << s.raw;
}

inline tcp_seq make_seq(uint32_t raw) { return tcp_seq{raw}; }
inline tcp_seq& operator+=(tcp_seq& s, int32_t n) { s.raw += n; return s; }
inline tcp_seq& operator-=(tcp_seq& s, int32_t n) { s.raw -= n; return s; }
inline tcp_seq operator+(tcp_seq s, int32_t n) { return s += n; }
inline tcp_seq operator-(tcp_seq s, int32_t n) { return s -= n; }
inline int32_t operator-(tcp_seq s, tcp_seq q) { return s.raw - q.raw; }
inline bool operator==(tcp_seq s, tcp_seq q)  { return s.raw == q.raw; }
inline bool operator!=(tcp_seq s, tcp_seq q) { return !(s == q); }
inline bool operator<(tcp_seq s, tcp_seq q) { return s - q < 0; }
inline bool operator>(tcp_seq s, tcp_seq q) { return q < s; }
inline bool operator<=(tcp_seq s, tcp_seq q) { return !(s > q); }
inline bool operator>=(tcp_seq s, tcp_seq q) { return !(s < q); }

struct tcp_hdr {
    packed<uint16_t> src_port;
    packed<uint16_t> dst_port;
    packed<tcp_seq> seq;
    packed<tcp_seq> ack;
    uint8_t rsvd1 : 4;
    uint8_t data_offset : 4;
    uint8_t f_fin : 1;
    uint8_t f_syn : 1;
    uint8_t f_rst : 1;
    uint8_t f_psh : 1;
    uint8_t f_ack : 1;
    uint8_t f_urg : 1;
    uint8_t rsvd2 : 2;
    packed<uint16_t> window;
    packed<uint16_t> checksum;
    packed<uint16_t> urgent;

    template <typename Adjuster>
    void adjust_endianness(Adjuster a) { a(src_port, dst_port, seq, ack, window, checksum, urgent); }
} __attribute__((packed));

template <typename InetTraits>
class tcp {
public:
    using ipaddr = typename InetTraits::address_type;
    using inet_type = typename InetTraits::inet_type;
    using connid = l4connid<InetTraits>;
    using connid_hash = typename connid::connid_hash;
    class connection;
    class listener;
private:
    class tcb;

    class tcb : public enable_lw_shared_from_this<tcb> {
        using clock_type = lowres_clock;
        static constexpr const tcp_state CLOSED         = tcp_state::CLOSED;
        static constexpr const tcp_state LISTEN         = tcp_state::LISTEN;
        static constexpr const tcp_state SYN_SENT       = tcp_state::SYN_SENT;
        static constexpr const tcp_state SYN_RECEIVED   = tcp_state::SYN_RECEIVED;
        static constexpr const tcp_state ESTABLISHED    = tcp_state::ESTABLISHED;
        static constexpr const tcp_state FIN_WAIT_1     = tcp_state::FIN_WAIT_1;
        static constexpr const tcp_state FIN_WAIT_2     = tcp_state::FIN_WAIT_2;
        static constexpr const tcp_state CLOSE_WAIT     = tcp_state::CLOSE_WAIT;
        static constexpr const tcp_state CLOSING        = tcp_state::CLOSING;
        static constexpr const tcp_state LAST_ACK       = tcp_state::LAST_ACK;
        static constexpr const tcp_state TIME_WAIT      = tcp_state::TIME_WAIT;
        tcp_state _state = CLOSED;
        tcp& _tcp;
        connection* _conn = nullptr;
        promise<> _connect_done;
        ipaddr _local_ip;
        ipaddr _foreign_ip;
        uint16_t _local_port;
        uint16_t _foreign_port;
        struct unacked_segment {
            packet p;
            uint16_t data_len;
            uint16_t data_remaining;
            unsigned nr_transmits;
            clock_type::time_point tx_time;
        };
        struct send {
            tcp_seq unacknowledged;
            tcp_seq next;
            uint32_t window;
            uint8_t window_scale;
            uint16_t mss;
            tcp_seq urgent;
            tcp_seq wl1;
            tcp_seq wl2;
            tcp_seq initial;
            std::deque<unacked_segment> data;
            std::deque<packet> unsent;
            uint32_t unsent_len = 0;
            uint32_t queued_len = 0;
            bool closed = false;
            promise<> _window_opened;
            // Wait for all data are acked
            std::experimental::optional<promise<>> _all_data_acked_promise;
            // Limit number of data queued into send queue
            semaphore user_queue_space = {212992};
            // Round-trip time variation
            std::chrono::milliseconds rttvar;
            // Smoothed round-trip time
            std::chrono::milliseconds srtt;
            bool first_rto_sample = true;
            clock_type::time_point syn_tx_time;
            // Congestion window
            uint32_t cwnd;
            // Slow start threshold
            uint32_t ssthresh;
            // Duplicated ACKs
            uint16_t dupacks = 0;
            unsigned syn_retransmit = 0;
            unsigned fin_retransmit = 0;
            uint32_t limited_transfer = 0;
            uint32_t partial_ack = 0;
            tcp_seq recover;
            bool window_probe = false;
        } _snd;
        struct receive {
            tcp_seq next;
            uint32_t window;
            uint8_t window_scale;
            uint16_t mss;
            tcp_seq urgent;
            tcp_seq initial;
            std::deque<packet> data;
            packet_merger<tcp_seq> out_of_order;
            std::experimental::optional<promise<>> _data_received_promise;
        } _rcv;
        tcp_option _option;
        timer<lowres_clock> _delayed_ack;
        // Retransmission timeout
        std::chrono::milliseconds _rto{1000};
        std::chrono::milliseconds _persist_time_out{1000};
        static constexpr std::chrono::milliseconds _rto_min{1000};
        static constexpr std::chrono::milliseconds _rto_max{60000};
        // Clock granularity
        static constexpr std::chrono::milliseconds _rto_clk_granularity{1};
        static constexpr uint16_t _max_nr_retransmit{5};
        timer<lowres_clock> _retransmit;
        timer<lowres_clock> _persist;
        uint16_t _nr_full_seg_received = 0;
        struct isn_secret {
            // 512 bits secretkey for ISN generating
            uint32_t key[16];
            isn_secret () {
                std::random_device rd;
                std::default_random_engine e(rd());
                std::uniform_int_distribution<uint32_t> dist{};
                for (auto& k : key) {
                    k = dist(e);
                }
            }
        };
        static isn_secret _isn_secret;
        tcp_seq get_isn();
        circular_buffer<typename InetTraits::l4packet> _packetq;
        bool _poll_active = false;
    public:
        tcb(tcp& t, connid id);
        void input_handle_listen_state(tcp_hdr* th, packet p);
        void input_handle_syn_sent_state(tcp_hdr* th, packet p);
        void input_handle_other_state(tcp_hdr* th, packet p);
        void output_one();
        future<> wait_for_data();
        future<> wait_for_all_data_acked();
        future<> send(packet p);
        void connect();
        packet read();
        void close();
        void remove_from_tcbs() {
            auto id = connid{_local_ip, _foreign_ip, _local_port, _foreign_port};
            _tcp._tcbs.erase(id);
        }
        std::experimental::optional<typename InetTraits::l4packet> get_packet();
        void output() {
            if (!_poll_active) {
                _poll_active = true;
                _tcp.poll_tcb(_foreign_ip, this->shared_from_this());
            }
        }
        future<> connect_done() {
            return _connect_done.get_future();
        }
        tcp_state& state() {
            return _state;
        }
    private:
        void respond_with_reset(tcp_hdr* th);
        bool merge_out_of_order();
        void insert_out_of_order(tcp_seq seq, packet p);
        void trim_receive_data_after_window();
        bool should_send_ack(uint16_t seg_len);
        void clear_delayed_ack();
        packet get_transmit_packet();
        void start_retransmit_timer() {
            auto now = clock_type::now();
            start_retransmit_timer(now);
        };
        void start_retransmit_timer(clock_type::time_point now) {
            auto tp = now + _rto;
            _retransmit.rearm(tp);
        };
        void stop_retransmit_timer() {
            _retransmit.cancel();
        };
        void start_persist_timer() {
            auto now = clock_type::now();
            start_persist_timer(now);
        };
        void start_persist_timer(clock_type::time_point now) {
            auto tp = now + _persist_time_out;
            _persist.rearm(tp);
        };
        void stop_persist_timer() {
            _persist.cancel();
        };
        void persist();
        void retransmit();
        void fast_retransmit();
        void update_rto(clock_type::time_point tx_time);
        void update_cwnd(uint32_t acked_bytes);
        void cleanup();
        uint32_t can_send() {
            if (_snd.window_probe) {
                return 1;
            }
            // Can not send more than advertised window allows
            auto x = std::min(uint32_t(_snd.unacknowledged + _snd.window - _snd.next), _snd.unsent_len);
            // Can not send more than congestion window allows
            x = std::min(_snd.cwnd, x);
            if (_snd.dupacks == 1 || _snd.dupacks == 2) {
                // RFC5681 Step 3.1
                // Send cwnd + 2 * smss per RFC3042
                auto flight = flight_size();
                auto max = _snd.cwnd + 2 * _snd.mss;
                x = flight <= max ? std::min(x, max - flight) : 0;
                _snd.limited_transfer += x;
            } else if (_snd.dupacks >= 3) {
                // RFC5681 Step 3.5
                // Sent 1 full-sized segment at most
                x = std::min(uint32_t(_snd.mss), x);
            }
            return x;
        }
        uint32_t flight_size() {
            uint32_t size = 0;
            std::for_each(_snd.data.begin(), _snd.data.end(), [&] (unacked_segment& seg) { size += seg.data_remaining; });
            return size;
        }
        uint16_t local_mss() {
            return _tcp.hw_features().mtu - net::tcp_hdr_len_min - InetTraits::ip_hdr_len_min;
        }
        void queue_packet(packet p) {
            _packetq.emplace_back(typename InetTraits::l4packet{_foreign_ip, std::move(p)});
        }
        void signal_data_received() {
            if (_rcv._data_received_promise) {
                _rcv._data_received_promise->set_value();
                _rcv._data_received_promise = {};
            }
        }
        void signal_all_data_acked() {
            if (_snd._all_data_acked_promise && _snd.unsent_len == 0 && _snd.queued_len == 0) {
                _snd._all_data_acked_promise->set_value();
                _snd._all_data_acked_promise = {};
            }
        }
        void do_syn_sent() {
            _state = SYN_SENT;
            _snd.syn_tx_time = clock_type::now();
            // Send <SYN> to remote
            output();
        }
        void do_syn_received() {
            _state = SYN_RECEIVED;
            _snd.syn_tx_time = clock_type::now();
            // Send <SYN,ACK> to remote
            output();
        }
        void do_established() {
            _state = ESTABLISHED;
            update_rto(_snd.syn_tx_time);
            _connect_done.set_value();
        }
        void do_reset() {
            _state = CLOSED;
            // Free packets to be sent which are waiting for _snd.user_queue_space
            _snd.user_queue_space.broken(tcp_reset_error());
            cleanup();
            if (_rcv._data_received_promise) {
                _rcv._data_received_promise->set_exception(tcp_reset_error());
            }
            if (_snd._all_data_acked_promise) {
                _snd._all_data_acked_promise->set_exception(tcp_reset_error());
            }
        }
        void do_time_wait() {
            // FIXME: Implement TIME_WAIT state timer
            _state = TIME_WAIT;
            cleanup();
        }
        void do_closed() {
            _state = CLOSED;
            cleanup();
        }
        void do_setup_isn() {
            _snd.initial = get_isn();
            _snd.unacknowledged = _snd.initial;
            _snd.next = _snd.initial + 1;
            _snd.recover = _snd.initial;
        }
        void do_local_fin_acked() {
            _snd.unacknowledged += 1;
            _snd.next += 1;
        }
        bool syn_needs_on() {
            return in_state(SYN_SENT | SYN_RECEIVED);
        }
        bool fin_needs_on() {
            return in_state(FIN_WAIT_1 | CLOSING | LAST_ACK) && _snd.closed &&
                   _snd.unsent_len == 0 && _snd.queued_len == 0;
        }
        bool ack_needs_on() {
            return !in_state(CLOSED | LISTEN | SYN_SENT);
        }
        bool foreign_will_not_send() {
            return in_state(CLOSING | TIME_WAIT | CLOSE_WAIT | LAST_ACK | CLOSED);
        }
        bool in_state(tcp_state state) {
            return uint16_t(_state) & uint16_t(state);
        }
        void exit_fast_recovery() {
            _snd.dupacks = 0;
            _snd.limited_transfer = 0;
            _snd.partial_ack = 0;
        }
        uint32_t data_segment_acked(tcp_seq seg_ack);
        bool segment_acceptable(tcp_seq seg_seq, unsigned seg_len);
        void init_from_options(tcp_hdr* th, uint8_t* opt_start, uint8_t* opt_end);
        friend class connection;
    };
    inet_type& _inet;
    std::unordered_map<connid, lw_shared_ptr<tcb>, connid_hash> _tcbs;
    std::unordered_map<uint16_t, listener*> _listening;
    std::random_device _rd;
    std::default_random_engine _e;
    std::uniform_int_distribution<uint16_t> _port_dist{41952, 65535};
    circular_buffer<std::pair<lw_shared_ptr<tcb>, ethernet_address>> _poll_tcbs;
    // queue for packets that do not belong to any tcb
    circular_buffer<ipv4_traits::l4packet> _packetq;
    semaphore _queue_space = {212992};
public:
    class connection {
        lw_shared_ptr<tcb> _tcb;
    public:
        explicit connection(lw_shared_ptr<tcb> tcbp) : _tcb(std::move(tcbp)) { _tcb->_conn = this; }
        connection(const connection&) = delete;
        connection(connection&& x) noexcept : _tcb(std::move(x._tcb)) {
            _tcb->_conn = this;
        }
        ~connection();
        void operator=(const connection&) = delete;
        connection& operator=(connection&& x) {
            if (this != &x) {
                this->~connection();
                new (this) connection(std::move(x));
            }
            return *this;
        }
        future<> send(packet p) {
            return _tcb->send(std::move(p));
        }
        future<> wait_for_data() {
            return _tcb->wait_for_data();
        }
        packet read() {
            return _tcb->read();
        }
        void close_read();
        void close_write();
    };
    class listener {
        tcp& _tcp;
        uint16_t _port;
        queue<connection> _q;
    private:
        listener(tcp& t, uint16_t port, size_t queue_length)
            : _tcp(t), _port(port), _q(queue_length) {
            _tcp._listening.emplace(_port, this);
        }
    public:
        listener(listener&& x)
            : _tcp(x._tcp), _port(x._port), _q(std::move(x._q)) {
            _tcp._listening[_port] = this;
            x._port = 0;
        }
        ~listener() {
            if (_port) {
                _tcp._listening.erase(_port);
            }
        }
        future<connection> accept() {
            return _q.not_empty().then([this] {
                return make_ready_future<connection>(_q.pop());
            });
        }
        friend class tcp;
    };
public:
    explicit tcp(inet_type& inet);
    void received(packet p, ipaddr from, ipaddr to);
    bool forward(forward_hash& out_hash_data, packet& p, size_t off);
    listener listen(uint16_t port, size_t queue_length = 100);
    future<connection> connect(socket_address sa);
    net::hw_features hw_features() { return _inet._inet.hw_features(); }
    void poll_tcb(ipaddr to, lw_shared_ptr<tcb> tcb);
private:
    void send_packet_without_tcb(ipaddr from, ipaddr to, packet p);
    void respond_with_reset(tcp_hdr* rth, ipaddr local_ip, ipaddr foreign_ip);
    friend class listener;
};

template <typename InetTraits>
tcp<InetTraits>::tcp(inet_type& inet) : _inet(inet), _e(_rd()) {
    _inet.register_packet_provider([this, tcb_polled = 0u] () mutable {
        std::experimental::optional<typename InetTraits::l4packet> l4p;
        auto c = _poll_tcbs.size();
        if (!_packetq.empty() && (!(tcb_polled % 128) || c == 0)) {
            l4p = std::move(_packetq.front());
            _packetq.pop_front();
            _queue_space.signal(l4p.value().p.len());
        } else {
            while (c--) {
                tcb_polled++;
                lw_shared_ptr<tcb> tcb;
                ethernet_address dst;
                std::tie(tcb, dst) = std::move(_poll_tcbs.front());
                _poll_tcbs.pop_front();
                l4p = tcb->get_packet();
                if (l4p) {
                    l4p.value().e_dst = dst;
                    break;
                }
            }
        }
        return l4p;
    });
}

template <typename InetTraits>
void tcp<InetTraits>::poll_tcb(ipaddr to, lw_shared_ptr<tcb> tcb) {
    _inet.get_l2_dst_address(to).then([this, tcb = std::move(tcb)] (ethernet_address dst) {
        _poll_tcbs.emplace_back(std::move(tcb), dst);
    });
}

template <typename InetTraits>
auto tcp<InetTraits>::listen(uint16_t port, size_t queue_length) -> listener {
    return listener(*this, port, queue_length);
}

template <typename InetTraits>
future<typename tcp<InetTraits>::connection> tcp<InetTraits>::connect(socket_address sa) {
    uint16_t src_port;
    connid id;
    auto src_ip = _inet._inet.host_address();
    auto dst_ip = ipv4_address(sa);
    auto dst_port = net::ntoh(sa.u.in.sin_port);

    do {
        src_port = _port_dist(_e);
        id = connid{src_ip, dst_ip, src_port, dst_port};
    } while (_inet._inet.netif()->hash2cpu(id.hash()) != engine().cpu_id()
            || _tcbs.find(id) != _tcbs.end());

    auto tcbp = make_lw_shared<tcb>(*this, id);
    _tcbs.insert({id, tcbp});
    tcbp->connect();

    return tcbp->connect_done().then([tcbp] {
        return make_ready_future<connection>(connection(tcbp));
    });
}

template <typename InetTraits>
bool tcp<InetTraits>::forward(forward_hash& out_hash_data, packet& p, size_t off) {
    auto th = p.get_header<tcp_hdr>(off);
    if (th) {
        out_hash_data.push_back(th->src_port);
        out_hash_data.push_back(th->dst_port);
    }
    return true;
}

template <typename InetTraits>
void tcp<InetTraits>::received(packet p, ipaddr from, ipaddr to) {
    auto th = p.get_header<tcp_hdr>(0);
    if (!th) {
        return;
    }
    // th->data_offset is correct even before ntoh()
    if (unsigned(th->data_offset * 4) < sizeof(*th)) {
        return;
    }

    if (!hw_features().rx_csum_offload) {
        checksummer csum;
        InetTraits::tcp_pseudo_header_checksum(csum, from, to, p.len());
        csum.sum(p);
        if (csum.get() != 0) {
            return;
        }
    }
    auto h = ntoh(*th);
    auto id = connid{to, from, h.dst_port, h.src_port};
    auto tcbi = _tcbs.find(id);
    lw_shared_ptr<tcb> tcbp;
    if (tcbi == _tcbs.end()) {
        auto listener = _listening.find(id.local_port);
        if (listener == _listening.end() || listener->second->_q.full()) {
            // 1) In CLOSE state
            // 1.1 all data in the incoming segment is discarded.  An incoming
            // segment containing a RST is discarded. An incoming segment not
            // containing a RST causes a RST to be sent in response.
            // FIXME:
            //      if ACK off: <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>
            //      if ACK on:  <SEQ=SEG.ACK><CTL=RST>
            return respond_with_reset(&h, id.local_ip, id.foreign_ip);
        } else {
            // 2) In LISTEN state
            // 2.1 first check for an RST
            if (h.f_rst) {
                // An incoming RST should be ignored
                return;
            }
            // 2.2 second check for an ACK
            if (h.f_ack) {
                // Any acknowledgment is bad if it arrives on a connection
                // still in the LISTEN state.
                // <SEQ=SEG.ACK><CTL=RST>
                return respond_with_reset(&h, id.local_ip, id.foreign_ip);
            }
            // 2.3 third check for a SYN
            if (h.f_syn) {
                // check the security
                // NOTE: Ignored for now
                tcbp = make_lw_shared<tcb>(*this, id);
                listener->second->_q.push(connection(tcbp));
                _tcbs.insert({id, tcbp});
                return tcbp->input_handle_listen_state(&h, std::move(p));
            }
            // 2.4 fourth other text or control
            // So you are unlikely to get here, but if you do, drop the
            // segment, and return.
            return;
        }
    } else {
        tcbp = tcbi->second;
        if (tcbp->state() == tcp_state::SYN_SENT) {
            // 3) In SYN_SENT State
            return tcbp->input_handle_syn_sent_state(&h, std::move(p));
        } else {
            // 4) In other state, can be one of the following:
            // SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2
            // CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT
            return tcbp->input_handle_other_state(&h, std::move(p));
        }
    }
}

// Send packet does not belong to any tcb
template <typename InetTraits>
void tcp<InetTraits>::send_packet_without_tcb(ipaddr from, ipaddr to, packet p) {
    if (_queue_space.try_wait(p.len())) { // drop packets that do not fit the queue
        _inet.get_l2_dst_address(to).then([this, to, p = std::move(p)] (ethernet_address e_dst) mutable {
                _packetq.emplace_back(ipv4_traits::l4packet{to, std::move(p), e_dst, ip_protocol_num::tcp});
        });
    }
}

template <typename InetTraits>
tcp<InetTraits>::connection::~connection() {
    if (_tcb) {
        _tcb->_conn = nullptr;
        close_read();
        close_write();
    }
}

template <typename InetTraits>
tcp<InetTraits>::tcb::tcb(tcp& t, connid id)
    : _tcp(t)
    , _local_ip(id.local_ip)
    , _foreign_ip(id.foreign_ip)
    , _local_port(id.local_port)
    , _foreign_port(id.foreign_port)
    , _delayed_ack([this] { _nr_full_seg_received = 0; output(); })
    , _retransmit([this] { retransmit(); })
    , _persist([this] { persist(); }) {
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::respond_with_reset(tcp_hdr* rth) {
    _tcp.respond_with_reset(rth, _local_ip, _foreign_ip);
}

template <typename InetTraits>
void tcp<InetTraits>::respond_with_reset(tcp_hdr* rth, ipaddr local_ip, ipaddr foreign_ip) {
    if (rth->f_rst) {
        return;
    }
    packet p;
    auto th = p.prepend_header<tcp_hdr>();
    th->src_port = rth->dst_port;
    th->dst_port = rth->src_port;
    if (rth->f_ack) {
        th->seq = rth->ack;
    }
    // If this RST packet is in response to a SYN packet. We ACK the ISN.
    if (rth->f_syn) {
        th->ack = rth->seq + 1;
        th->f_ack = true;
    }
    th->f_rst = true;
    th->data_offset = sizeof(*th) / 4;
    th->checksum = 0;
    *th = hton(*th);

    checksummer csum;
    InetTraits::tcp_pseudo_header_checksum(csum, local_ip, foreign_ip, sizeof(*th));
    if (hw_features().tx_csum_l4_offload) {
        th->checksum = ~csum.get();
    } else {
        csum.sum(p);
        th->checksum = csum.get();
    }

    offload_info oi;
    oi.protocol = ip_protocol_num::tcp;
    oi.tcp_hdr_len = sizeof(tcp_hdr);
    p.set_offload_info(oi);

    send_packet_without_tcb(local_ip, foreign_ip, std::move(p));
}

template <typename InetTraits>
uint32_t tcp<InetTraits>::tcb::data_segment_acked(tcp_seq seg_ack) {
    uint32_t total_acked_bytes = 0;
    // Full ACK of segment
    while (!_snd.data.empty()
            && (_snd.unacknowledged + _snd.data.front().data_remaining <= seg_ack)) {
        auto acked_bytes = _snd.data.front().data_remaining;
        _snd.unacknowledged += acked_bytes;
        // Ignore retransmitted segments when setting the RTO
        if (_snd.data.front().nr_transmits == 0) {
            update_rto(_snd.data.front().tx_time);
        }
        update_cwnd(acked_bytes);
        total_acked_bytes += acked_bytes;
        _snd.user_queue_space.signal(_snd.data.front().data_len);
        _snd.data.pop_front();
    }
    // Partial ACK of segment
    if (_snd.unacknowledged < seg_ack) {
        // For simplicity sake, do not trim the partially acked data
        // off the unacked segment. So we do not need to recalculate
        // the tcp header when retransmit the segment, but the downside
        // is that we will retransmit the whole segment although part
        // of them are already acked.
        auto acked_bytes = seg_ack - _snd.unacknowledged;
        _snd.data.front().data_remaining -= acked_bytes;
        _snd.unacknowledged = seg_ack;
        update_cwnd(acked_bytes);
        total_acked_bytes += acked_bytes;
    }
    return total_acked_bytes;
}

template <typename InetTraits>
bool tcp<InetTraits>::tcb::segment_acceptable(tcp_seq seg_seq, unsigned seg_len) {
    if (seg_len == 0 && _rcv.window == 0) {
        // SEG.SEQ = RCV.NXT
        return seg_seq == _rcv.next;
    } else if (seg_len == 0 && _rcv.window > 0) {
        // RCV.NXT =< SEG.SEQ < RCV.NXT+RCV.WND
        return (_rcv.next <= seg_seq) && (seg_seq < _rcv.next + _rcv.window);
    } else if (seg_len > 0 && _rcv.window > 0) {
        // RCV.NXT =< SEG.SEQ < RCV.NXT+RCV.WND
        //    or
        // RCV.NXT =< SEG.SEQ+SEG.LEN-1 < RCV.NXT+RCV.WND
        bool x = (_rcv.next <= seg_seq) && seg_seq < (_rcv.next + _rcv.window);
        bool y = (_rcv.next <= seg_seq + seg_len - 1) && (seg_seq + seg_len - 1 < _rcv.next + _rcv.window);
        return x || y;
    } else  {
        // SEG.LEN > 0 RCV.WND = 0, not acceptable
        return false;
    }
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::init_from_options(tcp_hdr* th, uint8_t* opt_start, uint8_t* opt_end) {
    // Handle tcp options
    _option.parse(opt_start, opt_end);

    // Remote receive window scale factor
    _snd.window_scale = _option._remote_win_scale;
    // Local receive window scale factor
    _rcv.window_scale = _option._local_win_scale;

    // Maximum segment size remote can receive
    _snd.mss = _option._remote_mss;
    // Maximum segment size local can receive
    _rcv.mss = _option._local_mss = local_mss();

    // Linux's default window size
    _rcv.window = 29200 << _rcv.window_scale;
    _snd.window = th->window << _snd.window_scale;

    // Segment sequence number used for last window update
    _snd.wl1 = th->seq;
    // Segment acknowledgment number used for last window update
    _snd.wl2 = th->ack;

    // Setup initial congestion window
    if (2190 < _snd.mss) {
        _snd.cwnd = 2 * _snd.mss;
    } else if (1095 < _snd.mss && _snd.mss <= 2190) {
        _snd.cwnd = 3 * _snd.mss;
    } else {
        _snd.cwnd = 4 * _snd.mss;
    }

    // Setup initial slow start threshold
    _snd.ssthresh = th->window << _snd.window_scale;
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::input_handle_listen_state(tcp_hdr* th, packet p) {
    auto opt_start = p.get_header<uint8_t>(sizeof(tcp_hdr));
    auto opt_end = opt_start + th->data_offset * 4;
    p.trim_front(th->data_offset * 4);
    tcp_seq seg_seq = th->seq;

    // Set RCV.NXT to SEG.SEQ+1, IRS is set to SEG.SEQ
    _rcv.next = seg_seq + 1;
    _rcv.initial = seg_seq;

    // ISS should be selected and a SYN segment sent of the form:
    // <SEQ=ISS><ACK=RCV.NXT><CTL=SYN,ACK>
    // SND.NXT is set to ISS+1 and SND.UNA to ISS
    // NOTE: In previous code, _snd.next is set to ISS + 1 only when SYN is
    // ACKed. Now, we set _snd.next to ISS + 1 here, so in output_one(): we
    // have
    //     th->seq = syn_on ? _snd.initial : _snd.next
    // to make sure retransmitted SYN has correct SEQ number.
    do_setup_isn();

    _rcv.urgent = _rcv.next;

    tcp_debug("listen: LISTEN -> SYN_RECEIVED\n");
    init_from_options(th, opt_start, opt_end);
    do_syn_received();
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::input_handle_syn_sent_state(tcp_hdr* th, packet p) {
    auto opt_start = p.get_header<uint8_t>(sizeof(tcp_hdr));
    auto opt_end = opt_start + th->data_offset * 4;
    p.trim_front(th->data_offset * 4);
    tcp_seq seg_seq = th->seq;
    auto seg_ack = th->ack;

    bool acceptable = false;
    // 3.1 first check the ACK bit
    if (th->f_ack) {
        // If SEG.ACK =< ISS, or SEG.ACK > SND.NXT, send a reset (unless the
        // RST bit is set, if so drop the segment and return)
        if (seg_ack <= _snd.initial || seg_ack > _snd.next) {
            return respond_with_reset(th);
        }

        // If SND.UNA =< SEG.ACK =< SND.NXT then the ACK is acceptable.
        acceptable = _snd.unacknowledged <= seg_ack && seg_ack <= _snd.next;
    }

    // 3.2 second check the RST bit
    if (th->f_rst) {
        // If the ACK was acceptable then signal the user "error: connection
        // reset", drop the segment, enter CLOSED state, delete TCB, and
        // return.  Otherwise (no ACK) drop the segment and return.
        if (acceptable) {
            return do_reset();
        } else {
            return;
        }
    }

    // 3.3 third check the security and precedence
    // NOTE: Ignored for now

    // 3.4 fourth check the SYN bit
    if (th->f_syn) {
        // RCV.NXT is set to SEG.SEQ+1, IRS is set to SEG.SEQ.  SND.UNA should
        // be advanced to equal SEG.ACK (if there is an ACK), and any segments
        // on the retransmission queue which are thereby acknowledged should be
        // removed.
        _rcv.next = seg_seq + 1;
        _rcv.initial = seg_seq;
        if (th->f_ack) {
            // TODO: clean retransmission queue
            _snd.unacknowledged = seg_ack;
        }
        if (_snd.unacknowledged > _snd.initial) {
            // If SND.UNA > ISS (our SYN has been ACKed), change the connection
            // state to ESTABLISHED, form an ACK segment
            // <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
            tcp_debug("syn: SYN_SENT -> ESTABLISHED\n");
            init_from_options(th, opt_start, opt_end);
            do_established();
            output();
        } else {
            // Otherwise enter SYN_RECEIVED, form a SYN,ACK segment
            // <SEQ=ISS><ACK=RCV.NXT><CTL=SYN,ACK>
            tcp_debug("syn: SYN_SENT -> SYN_RECEIVED\n");
            do_syn_received();
        }
    }

    // 3.5 fifth, if neither of the SYN or RST bits is set then drop the
    // segment and return.
    return;
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::input_handle_other_state(tcp_hdr* th, packet p) {
    p.trim_front(th->data_offset * 4);
    bool do_output = false;
    bool do_output_data = false;
    tcp_seq seg_seq = th->seq;
    auto seg_ack = th->ack;
    auto seg_len = p.len();

    // 4.1 first check sequence number
    if (!segment_acceptable(seg_seq, seg_len)) {
        //<SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
        return output();
    }

    // In the following it is assumed that the segment is the idealized
    // segment that begins at RCV.NXT and does not exceed the window.
    if (seg_seq < _rcv.next) {
        // ignore already acknowledged data
        auto dup = std::min(uint32_t(_rcv.next - seg_seq), seg_len);
        p.trim_front(dup);
        seg_len -= dup;
        seg_seq += dup;
    }
    // FIXME: We should trim data outside the right edge of the receive window as well

    if (seg_seq != _rcv.next) {
        insert_out_of_order(seg_seq, std::move(p));
        // A TCP receiver SHOULD send an immediate duplicate ACK
        // when an out-of-order segment arrives.
        return output();
    }

    // 4.2 second check the RST bit
    if (th->f_rst) {
        if (in_state(SYN_RECEIVED)) {
            // If this connection was initiated with a passive OPEN (i.e.,
            // came from the LISTEN state), then return this connection to
            // LISTEN state and return.  The user need not be informed.  If
            // this connection was initiated with an active OPEN (i.e., came
            // from SYN_SENT state) then the connection was refused, signal
            // the user "connection refused".  In either case, all segments
            // on the retransmission queue should be removed.  And in the
            // active OPEN case, enter the CLOSED state and delete the TCB,
            // and return.
            _connect_done.set_exception(tcp_refused_error());
            return do_reset();
        }
        if (in_state(ESTABLISHED | FIN_WAIT_1 | FIN_WAIT_2 | CLOSE_WAIT)) {
            // If the RST bit is set then, any outstanding RECEIVEs and SEND
            // should receive "reset" responses.  All segment queues should be
            // flushed.  Users should also receive an unsolicited general
            // "connection reset" signal.  Enter the CLOSED state, delete the
            // TCB, and return.
            return do_reset();
        }
        if (in_state(CLOSING | LAST_ACK | TIME_WAIT)) {
            // If the RST bit is set then, enter the CLOSED state, delete the
            // TCB, and return.
            return do_closed();
        }
    }

    // 4.3 third check security and precedence
    // NOTE: Ignored for now

    // 4.4 fourth, check the SYN bit
    if (th->f_syn) {
        // SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2
        // CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT

        // If the SYN is in the window it is an error, send a reset, any
        // outstanding RECEIVEs and SEND should receive "reset" responses,
        // all segment queues should be flushed, the user should also
        // receive an unsolicited general "connection reset" signal, enter
        // the CLOSED state, delete the TCB, and return.
        respond_with_reset(th);
        return do_reset();

        // If the SYN is not in the window this step would not be reached
        // and an ack would have been sent in the first step (sequence
        // number check).
    }

    // 4.5 fifth check the ACK field
    if (!th->f_ack) {
        // if the ACK bit is off drop the segment and return
        return;
    } else {
        // SYN_RECEIVED STATE
        if (in_state(SYN_RECEIVED)) {
            // If SND.UNA =< SEG.ACK =< SND.NXT then enter ESTABLISHED state
            // and continue processing.
            if (_snd.unacknowledged <= seg_ack && seg_ack <= _snd.next) {
                tcp_debug("SYN_RECEIVED -> ESTABLISHED\n");
                do_established();
            } else {
                // <SEQ=SEG.ACK><CTL=RST>
                return respond_with_reset(th);
            }
        }
        auto update_window = [this, th, seg_seq, seg_ack] {
            tcp_debug("window update seg_seq=%d, seg_ack=%d, old window=%d new window=%d\n",
                      seg_seq, seg_ack, _snd.window, th->window << _snd.window_scale);
            _snd.window = th->window << _snd.window_scale;
            _snd.wl1 = seg_seq;
            _snd.wl2 = seg_ack;
            if (_snd.window == 0) {
                _persist_time_out = _rto;
                start_persist_timer();
            } else {
                stop_persist_timer();
            }
        };
        // ESTABLISHED STATE or
        // CLOSE_WAIT STATE: Do the same processing as for the ESTABLISHED state.
        if (in_state(ESTABLISHED | CLOSE_WAIT)){
            // If SND.UNA < SEG.ACK =< SND.NXT then, set SND.UNA <- SEG.ACK.
            if (_snd.unacknowledged < seg_ack && seg_ack <= _snd.next) {
                // Remote ACKed data we sent
                auto acked_bytes = data_segment_acked(seg_ack);

                // If SND.UNA < SEG.ACK =< SND.NXT, the send window should be updated.
                if (_snd.wl1 < seg_seq || (_snd.wl1 == seg_seq && _snd.wl2 <= seg_ack)) {
                    update_window();
                }

                // some data is acked, try send more data
                do_output_data = true;

                auto set_retransmit_timer = [this] {
                    if (_snd.data.empty()) {
                        // All outstanding segments are acked, turn off the timer.
                        stop_retransmit_timer();
                        // Signal the waiter of this event
                        signal_all_data_acked();
                    } else {
                        // Restart the timer becasue new data is acked.
                        start_retransmit_timer();
                    }
                };

                if (_snd.dupacks >= 3) {
                    // We are in fast retransmit / fast recovery phase
                    uint32_t smss = _snd.mss;
                    if (seg_ack > _snd.recover) {
                        tcp_debug("ack: full_ack\n");
                        // Set cwnd to min (ssthresh, max(FlightSize, SMSS) + SMSS)
                        _snd.cwnd = std::min(_snd.ssthresh, std::max(flight_size(), smss) + smss);
                        // Exit the fast recovery procedure
                        exit_fast_recovery();
                        set_retransmit_timer();
                    } else {
                        tcp_debug("ack: partial_ack\n");
                        // Retransmit the first unacknowledged segment
                        fast_retransmit();
                        // Deflate the congestion window by the amount of new data
                        // acknowledged by the Cumulative Acknowledgment field
                        _snd.cwnd -= acked_bytes;
                        // If the partial ACK acknowledges at least one SMSS of new
                        // data, then add back SMSS bytes to the congestion window
                        if (acked_bytes >= smss) {
                            _snd.cwnd += smss;
                        }
                        // Send a new segment if permitted by the new value of
                        // cwnd.  Do not exit the fast recovery procedure For
                        // the first partial ACK that arrives during fast
                        // recovery, also reset the retransmit timer.
                        if (++_snd.partial_ack == 1) {
                            start_retransmit_timer();
                        }
                    }
                } else {
                    // RFC5681: The fast retransmit algorithm uses the arrival
                    // of 3 duplicate ACKs (as defined in section 2, without
                    // any intervening ACKs which move SND.UNA) as an
                    // indication that a segment has been lost.  
                    //
                    // So, here we reset dupacks to zero becasue this ACK moves
                    // SND.UNA.
                    exit_fast_recovery();
                    set_retransmit_timer();
                }
            } else if (!_snd.data.empty() && seg_len == 0 &&
                th->f_fin == 0 && th->f_syn == 0 &&
                th->ack == _snd.unacknowledged &&
                uint32_t(th->window << _snd.window_scale) == _snd.window) {
                // Note:
                // RFC793 states:
                // If the ACK is a duplicate (SEG.ACK < SND.UNA), it can be ignored
                // RFC5681 states:
                // The TCP sender SHOULD use the "fast retransmit" algorithm to detect
                // and repair loss, based on incoming duplicate ACKs.
                // Here, We follow RFC5681.
                _snd.dupacks++;
                uint32_t smss = _snd.mss;
                // 3 duplicated ACKs trigger a fast retransmit
                if (_snd.dupacks == 1 || _snd.dupacks == 2) {
                    // RFC5681 Step 3.1
                    // Send cwnd + 2 * smss per RFC3042
                    do_output_data = true;
                } else if (_snd.dupacks == 3) {
                    // RFC6582 Step 3.2
                    if (seg_ack - 1 > _snd.recover) {
                        _snd.recover = _snd.next - 1;
                        // RFC5681 Step 3.2
                        _snd.ssthresh = std::max((flight_size() - _snd.limited_transfer) / 2, 2 * smss);
                        fast_retransmit();
                    } else {
                        // Do not enter fast retransmit and do not reset ssthresh
                    }
                    // RFC5681 Step 3.3
                    _snd.cwnd = _snd.ssthresh + 3 * smss;
                } else if (_snd.dupacks > 3) {
                    // RFC5681 Step 3.4
                    _snd.cwnd += smss;
                    // RFC5681 Step 3.5
                    do_output_data = true;
                }
            } else if (seg_ack > _snd.next) {
                // If the ACK acks something not yet sent (SEG.ACK > SND.NXT)
                // then send an ACK, drop the segment, and return
                return output();
            } else if (_snd.window == 0 && th->window > 0) {
                update_window();
                do_output_data = true;
            }
        }
        // FIN_WAIT_1 STATE
        if (in_state(FIN_WAIT_1)) {
            // In addition to the processing for the ESTABLISHED state, if
            // our FIN is now acknowledged then enter FIN-WAIT-2 and continue
            // processing in that state.
            if (seg_ack == _snd.next + 1) {
                tcp_debug("ack: FIN_WAIT_1 -> FIN_WAIT_2\n");
                _state = FIN_WAIT_2;
                do_local_fin_acked();
            }
        }
        // FIN_WAIT_2 STATE
        if (in_state(FIN_WAIT_2)) {
            // In addition to the processing for the ESTABLISHED state, if
            // the retransmission queue is empty, the user’s CLOSE can be
            // acknowledged ("ok") but do not delete the TCB.
            // TODO
        }
        // CLOSING STATE
        if (in_state(CLOSING)) {
            if (seg_ack == _snd.next + 1) {
                tcp_debug("ack: CLOSING -> TIME_WAIT\n");
                do_local_fin_acked();
                return do_time_wait();
            } else {
                return;
            }
        }
        // LAST_ACK STATE
        if (in_state(LAST_ACK)) {
            if (seg_ack == _snd.next + 1) {
                tcp_debug("ack: LAST_ACK -> CLOSED\n");
                do_local_fin_acked();
                return do_closed();
            }
        }
        // TIME_WAIT STATE
        if (in_state(TIME_WAIT)) {
            // The only thing that can arrive in this state is a
            // retransmission of the remote FIN. Acknowledge it, and restart
            // the 2 MSL timeout.
            // TODO
        }
    }

    // 4.6 sixth, check the URG bit
    if (th->f_urg) {
        // TODO
    }

    // 4.7 seventh, process the segment text
    if (in_state(ESTABLISHED | FIN_WAIT_1 | FIN_WAIT_1)) {
        if (p.len()) {
            // Once the TCP takes responsibility for the data it advances
            // RCV.NXT over the data accepted, and adjusts RCV.WND as
            // apporopriate to the current buffer availability.  The total of
            // RCV.NXT and RCV.WND should not be reduced.
            _rcv.data.push_back(std::move(p));
            _rcv.next += seg_len;
            auto merged = merge_out_of_order();
            signal_data_received();
            // Send an acknowledgment of the form:
            // <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
            // This acknowledgment should be piggybacked on a segment being
            // transmitted if possible without incurring undue delay.
            if (merged) {
                // TCP receiver SHOULD send an immediate ACK when the
                // incoming segment fills in all or part of a gap in the
                // sequence space.
                do_output = true;
            } else {
                do_output = should_send_ack(seg_len);
            }
        }
    } else if (in_state(CLOSE_WAIT | CLOSING | LAST_ACK | TIME_WAIT)) {
        // This should not occur, since a FIN has been received from the
        // remote side. Ignore the segment text.
        return;
    }

    // 4.8 eighth, check the FIN bit
    if (th->f_fin) {
        if (in_state(CLOSED | LISTEN | SYN_SENT)) {
            // Do not process the FIN if the state is CLOSED, LISTEN or SYN-SENT
            // since the SEG.SEQ cannot be validated; drop the segment and return.
            return;
        }
        auto fin_seq = seg_seq + seg_len;
        if (fin_seq == _rcv.next) {
            _rcv.next = fin_seq + 1;
            signal_data_received();

            // If this <FIN> packet contains data as well, we can ACK both data
            // and <FIN> in a single packet, so canncel the previous ACK.
            clear_delayed_ack();
            do_output = false;
            // Send ACK for the FIN!
            output();

            if (in_state(SYN_RECEIVED | ESTABLISHED)) {
                tcp_debug("fin: SYN_RECEIVED or ESTABLISHED -> CLOSE_WAIT\n");
                _state = CLOSE_WAIT;
            }
            if (in_state(FIN_WAIT_1)) {
                // If our FIN has been ACKed (perhaps in this segment), then
                // enter TIME-WAIT, start the time-wait timer, turn off the other
                // timers; otherwise enter the CLOSING state.
                // Note: If our FIN has been ACKed, we should be in FIN_WAIT_2
                // not FIN_WAIT_1 if we reach here.
                tcp_debug("fin: FIN_WAIT_1 -> CLOSING\n");
                _state = CLOSING;
            }
            if (in_state(FIN_WAIT_2)) {
                tcp_debug("fin: FIN_WAIT_2 -> TIME_WAIT\n");
                return do_time_wait();
            }
        }
    }
    if (do_output || (do_output_data && can_send())) {
        // Since we will do output, we can canncel scheduled delayed ACK.
        clear_delayed_ack();
        output();
    }
}

template <typename InetTraits>
packet tcp<InetTraits>::tcb::get_transmit_packet() {
    // easy case: empty queue
    if (_snd.unsent.empty()) {
        return packet();
    }
    auto can_send = this->can_send();
    // Max number of TCP payloads we can pass to NIC
    uint32_t len;
    if (_tcp.hw_features().tx_tso) {
        // FIXME: Info tap device the size of the splitted packet
        len = _tcp.hw_features().max_packet_len - net::tcp_hdr_len_min - InetTraits::ip_hdr_len_min;
    } else {
        len = std::min(uint16_t(_tcp.hw_features().mtu - net::tcp_hdr_len_min - InetTraits::ip_hdr_len_min), _snd.mss);
    }
    can_send = std::min(can_send, len);
    // easy case: one small packet
    if (_snd.unsent.size() == 1 && _snd.unsent.front().len() <= can_send) {
        auto p = std::move(_snd.unsent.front());
        _snd.unsent.pop_front();
        _snd.unsent_len -= p.len();
        return p;
    }
    // moderate case: need to split one packet
    if (_snd.unsent.front().len() > can_send) {
        auto p = _snd.unsent.front().share(0, can_send);
        _snd.unsent.front().trim_front(can_send);
        _snd.unsent_len -= p.len();
        return p;
    }
    // hard case: merge some packets, possibly split last
    auto p = std::move(_snd.unsent.front());
    _snd.unsent.pop_front();
    can_send -= p.len();
    while (!_snd.unsent.empty()
            && _snd.unsent.front().len() <= can_send) {
        can_send -= _snd.unsent.front().len();
        p.append(std::move(_snd.unsent.front()));
        _snd.unsent.pop_front();
    }
    if (!_snd.unsent.empty() && can_send) {
        auto& q = _snd.unsent.front();
        p.append(q.share(0, can_send));
        q.trim_front(can_send);
    }
    _snd.unsent_len -= p.len();
    return p;
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::output_one() {
    if (in_state(CLOSED)) {
        return;
    }

    packet p = get_transmit_packet();
    uint16_t len = p.len();
    bool syn_on = syn_needs_on();
    bool ack_on = ack_needs_on();

    auto options_size = _option.get_size(syn_on, ack_on);
    auto th = p.prepend_header<tcp_hdr>(options_size);

    th->src_port = _local_port;
    th->dst_port = _foreign_port;

    th->f_syn = syn_on;
    th->f_ack = ack_on;
    if (ack_on) {
        clear_delayed_ack();
    }
    th->f_urg = false;
    th->f_psh = false;

    th->seq = syn_on ? _snd.initial : _snd.next;
    th->ack = _rcv.next;
    th->data_offset = (sizeof(*th) + options_size) / 4;
    th->window = _rcv.window >> _rcv.window_scale;
    th->checksum = 0;

    _snd.next += len;

    // FIXME: does the FIN have to fit in the window?
    bool fin_on = fin_needs_on();
    th->f_fin = fin_on;

    // Add tcp options
    _option.fill(th, options_size);
    *th = hton(*th);

    offload_info oi;
    checksummer csum;
    InetTraits::tcp_pseudo_header_checksum(csum, _local_ip, _foreign_ip, sizeof(*th) + options_size + len);
    if (_tcp.hw_features().tx_csum_l4_offload) {
        // tx checksum offloading - both virtio-net's VIRTIO_NET_F_CSUM
        // dpdk's PKT_TX_TCP_CKSUM - requires th->checksum to be
        // initialized to ones' complement sum of the pseudo header.
        th->checksum = ~csum.get();
        oi.needs_csum = true;
    } else {
        csum.sum(p);
        th->checksum = csum.get();
        oi.needs_csum = false;
    }
    oi.protocol = ip_protocol_num::tcp;
    oi.tcp_hdr_len = sizeof(tcp_hdr) + options_size;
    p.set_offload_info(oi);

    if (len || syn_on || fin_on) {
        auto now = clock_type::now();
        if (len) {
            unsigned nr_transmits = 0;
            _snd.data.emplace_back(unacked_segment{p.share(), len, len, nr_transmits, now});
        }
        if (!_retransmit.armed()) {
            start_retransmit_timer(now);
        }
    }

    queue_packet(std::move(p));
}

template <typename InetTraits>
future<> tcp<InetTraits>::tcb::wait_for_data() {
    if (!_rcv.data.empty() || foreign_will_not_send()) {
        return make_ready_future<>();
    }
    _rcv._data_received_promise = promise<>();
    return _rcv._data_received_promise->get_future();
}

template <typename InetTraits>
future<> tcp<InetTraits>::tcb::wait_for_all_data_acked() {
    if (_snd.data.empty() && _snd.unsent_len == 0 && _snd.queued_len == 0) {
        return make_ready_future<>();
    }
    _snd._all_data_acked_promise = promise<>();
    return _snd._all_data_acked_promise->get_future();
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::connect() {
    // An initial send sequence number (ISS) is selected.  A SYN segment of the
    // form <SEQ=ISS><CTL=SYN> is sent.  Set SND.UNA to ISS, SND.NXT to ISS+1,
    // enter SYN-SENT state, and return.
    do_setup_isn();

    // Local receive window scale factor
    _rcv.window_scale = _option._local_win_scale = 7;
    // Maximum segment size local can receive
    _rcv.mss = _option._local_mss = local_mss();
    // Linux's default window size
    _rcv.window = 29200 << _rcv.window_scale;

    do_syn_sent();
}

template <typename InetTraits>
packet tcp<InetTraits>::tcb::read() {
    packet p;
    for (auto&& q : _rcv.data) {
        p.append(std::move(q));
    }
    _rcv.data.clear();
    return p;
}

template <typename InetTraits>
future<> tcp<InetTraits>::tcb::send(packet p) {
    // We can not send after the connection is closed
    assert(!_snd.closed);

    if (in_state(CLOSED)) {
        return make_exception_future<>(tcp_reset_error());
    }

    // TODO: Handle p.len() > max user_queue_space case
    auto len = p.len();
    _snd.queued_len += len;
    return _snd.user_queue_space.wait(len).then([this, zis = this->shared_from_this(), p = std::move(p)] () mutable {
        assert(!_snd.closed);
        _snd.unsent_len += p.len();
        _snd.queued_len -= p.len();
        _snd.unsent.push_back(std::move(p));
        if (can_send() > 0) {
            output();
        }
        return make_ready_future<>();
    });
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::close() {
    if (in_state(CLOSED) || _snd.closed) {
        return;
    }
    // TODO: We should return a future to upper layer
    wait_for_all_data_acked().then([this, zis = this->shared_from_this()] () mutable {
        _snd.closed = true;
        tcp_debug("close: unsent_len=%d\n", _snd.unsent_len);
        if (in_state(CLOSE_WAIT)) {
            tcp_debug("close: CLOSE_WAIT -> LAST_ACK\n");
            _state = LAST_ACK;
        } else if (in_state(ESTABLISHED)) {
            tcp_debug("close: ESTABLISHED -> FIN_WAIT_1\n");
            _state = FIN_WAIT_1;
        }
        // Send <FIN> to remote
        // Note: we call output_one to make sure a packet with FIN actually
        // sent out. If we only call output() and _packetq is not empty,
        // tcp::tcb::get_packet(), packet with FIN will not be generated.
        output_one();
        output();
    });
}

template <typename InetTraits>
bool tcp<InetTraits>::tcb::should_send_ack(uint16_t seg_len) {
    // We've received a TSO packet, do ack immediately
    if (seg_len > _rcv.mss) {
        _nr_full_seg_received = 0;
        _delayed_ack.cancel();
        return true;
    }

    // We've received a full sized segment, ack for every second full sized segment
    if (seg_len == _rcv.mss) {
        if (_nr_full_seg_received++ >= 1) {
            _nr_full_seg_received = 0;
            _delayed_ack.cancel();
            return true;
        }
    }

    // If the timer is armed and its callback hasn't been run.
    if (_delayed_ack.armed()) {
        return false;
    }

    // If the timer is not armed, schedule a delayed ACK.
    // The maximum delayed ack timer allowed by RFC1122 is 500ms, most
    // implementations use 200ms.
    _delayed_ack.arm(200ms);
    return false;
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::clear_delayed_ack() {
    _delayed_ack.cancel();
}

template <typename InetTraits>
bool tcp<InetTraits>::tcb::merge_out_of_order() {
    bool merged = false;
    if (_rcv.out_of_order.map.empty()) {
        return merged;
    }
    for (auto it = _rcv.out_of_order.map.begin(); it != _rcv.out_of_order.map.end();) {
        auto& p = it->second;
        auto seg_beg = it->first;
        auto seg_len = p.len();
        auto seg_end = seg_beg + seg_len;
        if (seg_beg <= _rcv.next && _rcv.next < seg_end) {
            // This segment has been received out of order and its previous
            // segment has been received now
            auto trim = _rcv.next - seg_beg;
            if (trim) {
                p.trim_front(trim);
                seg_len -= trim;
            }
            _rcv.next += seg_len;
            _rcv.data.push_back(std::move(p));
            // Since c++11, erase() always returns the value of the following element
            it = _rcv.out_of_order.map.erase(it);
            merged = true;
        } else if (_rcv.next >= seg_end) {
            // This segment has been receive already, drop it
            it = _rcv.out_of_order.map.erase(it);
        } else {
            // seg_beg > _rcv.need, can not merge. Note, seg_beg can grow only,
            // so we can stop looking here.
            it++;
            break;
        }
    }
    return merged;
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::insert_out_of_order(tcp_seq seg, packet p) {
    _rcv.out_of_order.merge(seg, std::move(p));
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::trim_receive_data_after_window() {
    abort();
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::persist() {
    print("persist timer fired\n");
    // Send 1 byte packet to probe peer's window size
    _snd.window_probe = true;
    output_one();
    _snd.window_probe = false;

    output();
    // Perform binary exponential back-off per RFC1122
    _persist_time_out = std::min(_persist_time_out * 2, _rto_max);
    start_persist_timer();
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::retransmit() {
    auto output_update_rto = [this] {
        output();
        // According to RFC6298, Update RTO <- RTO * 2 to perform binary exponential back-off
        this->_rto = std::min(this->_rto * 2, this->_rto_max);
        start_retransmit_timer();
    };

    // Retransmit SYN
    if (syn_needs_on()) {
        if (_snd.syn_retransmit++ < _max_nr_retransmit) {
            output_update_rto();
        } else {
            _connect_done.set_exception(tcp_connect_error());
            cleanup();
            return;
        }
    }

    // Retransmit FIN
    if (fin_needs_on()) {
        if (_snd.fin_retransmit++ < _max_nr_retransmit) {
            output_update_rto();
        } else {
            cleanup();
            return;
        }
    }

    // Retransmit Data
    if (_snd.data.empty()) {
        return;
    }

    // If there are unacked data, retransmit the earliest segment
    auto& unacked_seg = _snd.data.front();

    // According to RFC5681
    // Update ssthresh only for the first retransmit
    uint32_t smss = _snd.mss;
    if (unacked_seg.nr_transmits == 0) {
        _snd.ssthresh = std::max(flight_size() / 2, 2 * smss);
    }
    // RFC6582 Step 4
    _snd.recover = _snd.next - 1;
    // Start the slow start process
    _snd.cwnd = smss;
    // End fast recovery
    exit_fast_recovery();

    if (unacked_seg.nr_transmits < _max_nr_retransmit) {
        unacked_seg.nr_transmits++;
    } else {
        // Delete connection when max num of retransmission is reached
        cleanup();
        return;
    }
    // TODO: If the Path MTU changes, we need to split the segment if it is larger than current MSS
    queue_packet(unacked_seg.p.share());

    output_update_rto();
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::fast_retransmit() {
    if (!_snd.data.empty()) {
        auto& unacked_seg = _snd.data.front();
        unacked_seg.nr_transmits++;
        queue_packet(unacked_seg.p.share());
        output();
    }
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::update_rto(clock_type::time_point tx_time) {
    // Update RTO according to RFC6298
    auto R = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - tx_time);
    if (_snd.first_rto_sample) {
        _snd.first_rto_sample = false;
        // RTTVAR <- R/2
        // SRTT <- R
        _snd.rttvar = R / 2;
        _snd.srtt = R;
    } else {
        // RTTVAR <- (1 - beta) * RTTVAR + beta * |SRTT - R'|
        // SRTT <- (1 - alpha) * SRTT + alpha * R'
        // where alpha = 1/8 and beta = 1/4
        auto delta = _snd.srtt > R ? (_snd.srtt - R) : (R - _snd.srtt);
        _snd.rttvar = _snd.rttvar * 3 / 4 + delta / 4;
        _snd.srtt = _snd.srtt * 7 / 8 +  R / 8;
    }
    // RTO <- SRTT + max(G, K * RTTVAR)
    _rto =  _snd.srtt + std::max(_rto_clk_granularity, 4 * _snd.rttvar);

    // Make sure 1 sec << _rto << 60 sec
    _rto = std::max(_rto, _rto_min);
    _rto = std::min(_rto, _rto_max);
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::update_cwnd(uint32_t acked_bytes) {
    uint32_t smss = _snd.mss;
    if (_snd.cwnd < _snd.ssthresh) {
        // In slow start phase
        _snd.cwnd += std::min(acked_bytes, smss);
    } else {
        // In congestion avoidance phase
        uint32_t round_up = 1;
        _snd.cwnd += std::max(round_up, smss * smss / _snd.cwnd);
    }
}

template <typename InetTraits>
void tcp<InetTraits>::tcb::cleanup() {
    _snd.unsent.clear();
    _snd.data.clear();
    _rcv.out_of_order.map.clear();
    _rcv.data.clear();
    stop_retransmit_timer();
    clear_delayed_ack();
    remove_from_tcbs();
}

template <typename InetTraits>
tcp_seq tcp<InetTraits>::tcb::get_isn() {
    // Per RFC6528, TCP SHOULD generate its Initial Sequence Numbers
    // with the expression:
    //   ISN = M + F(localip, localport, remoteip, remoteport, secretkey)
    //   M is the 4 microsecond timer
    using namespace std::chrono;
    uint32_t hash[4];
    hash[0] = _local_ip.ip;
    hash[1] = _foreign_ip.ip;
    hash[2] = (_local_port << 16) + _foreign_port;
    hash[3] = _isn_secret.key[15];
    CryptoPP::Weak::MD5::Transform(hash, _isn_secret.key);
    auto seq = hash[0];
    auto m = duration_cast<microseconds>(clock_type::now().time_since_epoch());
    seq += m.count() / 4;
    return make_seq(seq);
}

template <typename InetTraits>
std::experimental::optional<typename InetTraits::l4packet> tcp<InetTraits>::tcb::get_packet() {
    _poll_active = false;
    if (_packetq.empty()) {
        output_one();
    }

    if (in_state(CLOSED)) {
        return std::experimental::optional<typename InetTraits::l4packet>();
    }

    assert(!_packetq.empty());

    auto p = std::move(_packetq.front());
    _packetq.pop_front();
    if (!_packetq.empty() || (_snd.dupacks < 3 && can_send() > 0)) {
        // If there are packets to send in the queue or tcb is allowed to send
        // more add tcp back to polling set to keep sending. In addition, dupacks >= 3
        // is an indication that an segment is lost, stop sending more in this case.
        output();
    }
    return std::move(p);
}

template <typename InetTraits>
void tcp<InetTraits>::connection::close_read() {
}

template <typename InetTraits>
void tcp<InetTraits>::connection::close_write() {
    _tcb->close();
}

template <typename InetTraits>
constexpr uint16_t tcp<InetTraits>::tcb::_max_nr_retransmit;

template <typename InetTraits>
constexpr std::chrono::milliseconds tcp<InetTraits>::tcb::_rto_min;

template <typename InetTraits>
constexpr std::chrono::milliseconds tcp<InetTraits>::tcb::_rto_max;

template <typename InetTraits>
constexpr std::chrono::milliseconds tcp<InetTraits>::tcb::_rto_clk_granularity;

template <typename InetTraits>
typename tcp<InetTraits>::tcb::isn_secret tcp<InetTraits>::tcb::_isn_secret;

}



#endif /* TCP_HH_ */
