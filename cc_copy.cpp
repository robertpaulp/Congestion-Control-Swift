// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include <math.h>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include "cc.h"
#include "queue.h"
#include <stdio.h>
#include "switch.h"
#include "ecn.h"
#include <chrono>

using namespace std;

////////////////////////////////////////////////////////////////
//  CC SOURCE. Aici este codul care ruleaza pe transmitatori. Tot ce avem nevoie pentru a implementa
//  un algoritm de congestion control se gaseste aici.
////////////////////////////////////////////////////////////////
int CCSrc::_global_node_count = 0;

double _cwnd_prev;
double _max_cwnd;
double _min_cwnd;
double _alpha;
double _beta;
simtime_picosec _lastDecrease;
double _ai;
double _fsRange;
double _max_mdfs;
double _target_delay;
double _base_delay;
double _hops;
double _hop_scale;
int _retransmit_count;
const int RETX_RESET_THRESHOLD = 3; // Example threshold, commonly set to 3
double _pacing_delay;
int _duplicate_acks;
uint32_t _last_no_ack;

unordered_map<uint32_t, simtime_picosec> sentTimestamps;
unordered_map<uint32_t, simtime_picosec> retransmitTimeouts;
simtime_picosec timeoutInterval;

CCSrc::CCSrc(EventList &eventlist)
    : EventSource(eventlist,"cc"), _flow(NULL)
{
    _mss = Packet::data_packet_size();
    _acks_received = 0;
    _nacks_received = 0;

    _highest_sent = 0;
    _next_decision = 0;
    _flow_started = false;
    _sink = 0;

    _cwnd = 10 * _mss;
    _max_cwnd = 100000 * _mss;
    _min_cwnd = _mss;
    _cwnd_prev = _cwnd;
    _ai = 10;
    _max_mdfs = 0.25; // Adjust this value to control the maximum decrease factor
    _fsRange = 2;
    _pacing_delay = 0;
    _target_delay = 10;
    _base_delay = 1;
    _hops = 3;
    _duplicate_acks = 0;
    _last_no_ack = 0;
    _hop_scale = 1;
    _retransmit_count = 0;
    _alpha = _fsRange / ((1 / sqrt(_min_cwnd)) - (1 / sqrt(_max_cwnd)));
    _beta = -1 * _alpha / sqrt(_max_cwnd);
    _lastDecrease = eventlist.now();

    _node_num = _global_node_count++;
    _nodename = "CCsrc " + to_string(_node_num);

    _ssthresh = 0xFFFFFFFFFF;
    _flightsize = 0;
    _flow._name = _nodename;
    setName(_nodename);
}

/* Start the flow of octets */
void CCSrc::startflow() {
    cout << "Start flow " << _flow._name << " at " << timeAsSec(eventlist().now()) << "s" << endl;
    _flow_started = true;
    _highest_sent = 0;
    _packets_sent = 0;

    while (_flightsize + _mss < _cwnd)
        send_packet();
}

/* Initialize connection to the sink host */
void CCSrc::connect(Route* routeout, Route* routeback, CCSink& sink, simtime_picosec starttime) {
    assert(routeout);
    _route = routeout;

    _sink = &sink;
    _flow._name = _name;
    _sink->connect(*this, routeback);

    eventlist().sourceIsPending(*this, starttime);
}

double clamp(double min_val, double val, double max_val) {
    return std::max(min_val, std::min(val, max_val));
}

void updateTargetDelay(double _cwnd_aux) {
    _target_delay = _base_delay + (_hops * _hop_scale);
    _target_delay += std::max(0.0, std::min(1 / sqrt(_cwnd_aux) + _beta, _fsRange));
}

bool can_decrease(simtime_picosec current_time, simtime_picosec rtt) {
    return (current_time - _lastDecrease) >= rtt;
}

void handleTimeout(double &_cwnd_aux, int &_retransmit_count, double _min_cwnd, double _max_mdfs, int RETX_RESET_THRESHOLD) {
    cout << "Timeout detected." << endl;

    // Drastically reduce the congestion window
    _retransmit_count++;
    if (_retransmit_count >= RETX_RESET_THRESHOLD) {
        _cwnd_aux = _min_cwnd;
    } else {
        _cwnd_aux = _cwnd_aux * (1 - _max_mdfs);
    }
}

void checkTimeouts(EventList& eventlist, unordered_map<uint32_t, simtime_picosec>& retransmitTimeouts, std::function<void(double&, int&, double, double, int)> handleTimeout, double &_cwnd, int &_retransmit_count, double _min_cwnd, double _max_mdfs, int RETX_RESET_THRESHOLD) {
    simtime_picosec currentTimestamp = eventlist.now();
    for (auto it = retransmitTimeouts.begin(); it != retransmitTimeouts.end(); ) {
        if (currentTimestamp > it->second) { // Check if current time exceeds timeout time
            handleTimeout(_cwnd, _retransmit_count, _min_cwnd, _max_mdfs, RETX_RESET_THRESHOLD); // Handle timeout for the packet
            it = retransmitTimeouts.erase(it); // Remove the entry from the map
        } else {
            ++it;
        }
    }
}

// Function called when a packet is dropped due to queue overflow
void CCSrc::processNack(const CCNack& nack) {
    _nacks_received++;
    _flightsize -= _mss;

    simtime_picosec currentTimestamp = eventlist().now();
    simtime_picosec sentTimestamp;
    simtime_picosec rtt;

    auto it = sentTimestamps.find(nack.ackno());
    if (it != sentTimestamps.end()) {
        sentTimestamp = it->second;
        rtt = currentTimestamp - sentTimestamp;
        sentTimestamps.erase(it);
    }

    _retransmit_count++;
    if (_retransmit_count >= RETX_RESET_THRESHOLD) {
        _cwnd = _min_cwnd;
    } else {
        if (can_decrease(currentTimestamp, rtt)) {
            _cwnd *= (1 - _beta);
        }
    }

    if (nack.ackno() >= _next_decision) {
        _cwnd *= std::max(1.0 - _beta * (rtt / currentTimestamp), 1.0 - _max_mdfs);
        _next_decision = _highest_sent + _cwnd;
    }
    _cwnd = clamp(_min_cwnd, _cwnd, _max_cwnd);
    if (_cwnd <= _cwnd_prev) {
        _lastDecrease = currentTimestamp;
    }

    if (_cwnd < 1) {
        _pacing_delay = timeAsMs(rtt) / _cwnd;
    } else {
        _pacing_delay = 0;
    }
}


void CCSrc::processAck(const CCAck& ack) {
    CCAck::seq_t ackno = ack.ackno();

    _acks_received++;
    _flightsize -= _mss;

    simtime_picosec currentTimestamp = eventlist().now();
    simtime_picosec sentTimestamp;
    simtime_picosec rtt;

    auto it = sentTimestamps.find(ackno);
    if (it != sentTimestamps.end()) {
        sentTimestamp = it->second;
        rtt = currentTimestamp - sentTimestamp;
        sentTimestamps.erase(it); 
    }

    if (ackno == _last_no_ack) {
        _duplicate_acks++;
    } else {
        _last_no_ack = ackno;
        _duplicate_acks = 0;
    }

    _retransmit_count = 0;
    if (_duplicate_acks >= 3) {
        if (can_decrease(currentTimestamp, rtt)) {
            _cwnd = (1 - _beta) * _cwnd;
        }
    } else {
        updateTargetDelay(_cwnd);
        if (ack.is_ecn_marked()) {
            if (ackno >= _next_decision) {
                if (can_decrease(currentTimestamp, rtt)) {
                    _cwnd *= (1 - _beta);
                }
                _next_decision = _highest_sent + _cwnd;
            }
        } else {
            if (_target_delay >= rtt) {
                // Additive Increase (AI)
                if (_cwnd >= 1) {
                    _cwnd += _ai * _mss * _acks_received / _cwnd;
                } else {
                    _cwnd += _ai * _mss * _acks_received;
                }
            } else {
                // Multiplicative Decrease (MD)
                if (can_decrease(currentTimestamp, rtt)) {
                    _cwnd *= std::max(1.0 - _beta * (_target_delay / rtt), 1.0 - _max_mdfs);
                }
            }
        }
    }
}

// Functia de receptie, in functie de ce primeste cheama processLoss sau processACK
void CCSrc::receivePacket(Packet& pkt) {
    if (!_flow_started) {
        return;
    }

    switch (pkt.type()) {
    case CCNACK:
        processNack((const CCNack&)pkt);
        pkt.free();
        break;
    case CCACK:
        processAck((const CCAck&)pkt);
        pkt.free();
        break;
    default:
        cout << "Got packet with type " << pkt.type() << endl;
        abort();
    }

    //now send packets!
    while (_flightsize + _mss < _cwnd)
        send_packet();
}

// Note: the data sequence number is the number of Byte1 of the packet, not the last byte.
// Functia care se este chemata pentru transmisia unui pachet
void CCSrc::send_packet() {
    CCPacket* p = NULL;
    assert(_flow_started);
    simtime_picosec currentTimestamp = eventlist().now();
    simtime_picosec timeoutTimestamp = currentTimestamp + timeoutInterval; // Calculate timeout time
    p = CCPacket::newpkt(*_route, _flow, _highest_sent + 1, _mss, currentTimestamp);
    sentTimestamps[_highest_sent + 1] = currentTimestamp;
    retransmitTimeouts[_highest_sent + 1] = timeoutTimestamp; // Record the timeout time

    _highest_sent += _mss;
    _packets_sent++;
    _flightsize += _mss;

    // Send the packet
    p->sendOn();
}

void CCSrc::doNextEvent() {
    if (!_flow_started) {
        startflow();
        return;
    }

    checkTimeouts(eventlist(), retransmitTimeouts, 
                  handleTimeout, 
                  _cwnd, _retransmit_count, 
                  _min_cwnd, _max_mdfs, RETX_RESET_THRESHOLD);

    // Send packets if there is space in the congestion window
    while (_flightsize + _mss < _cwnd)
        send_packet();
}

////////////////////////////////////////////////////////////////
//  CC SINK Aici este codul ce ruleaza pe receptor, in mare nu o sa aducem multe modificari
////////////////////////////////////////////////////////////////

/* Only use this constructor when there is only one for to this receiver */
CCSink::CCSink()
    : Logged("CCSINK"), _total_received(0)
{
    _src = 0;

    _nodename = "CCsink";
    _total_received = 0;
}

/* Connect a src to this sink. */
void CCSink::connect(CCSrc& src, Route* route)
{
    _src = &src;
    _route = route;
    setName(_src->_nodename);
}

// Receive a packet.
// seqno is the first byte of the new packet.
void CCSink::receivePacket(Packet& pkt) {
    switch (pkt.type()) {
    case CC:
        break;
    default:
        abort();
    }

    CCPacket *p = (CCPacket*)(&pkt);
    CCPacket::seq_t seqno = p->seqno();

    simtime_picosec ts = p->ts();
    //bool last_packet = ((CCPacket*)&pkt)->last_packet();

    if (pkt.header_only()) {
        send_nack(ts, seqno);

        p->free();

        //cout << "Wrong seqno received at CC SINK " << seqno << " expecting " << _cumulative_ack << endl;
        return;
    }

    _total_received += Packet::data_packet_size();

    bool ecn = (bool)(pkt.flags() & ECN_CE);

    send_ack(ts, seqno, ecn);
    // have we seen everything yet?
    pkt.free();
}

void CCSink::send_ack(simtime_picosec ts, CCPacket::seq_t ackno, bool ecn) {
    CCAck *ack = 0;
    ack = CCAck::newpkt(_src->_flow, *_route, ackno, ts, ecn);
    ack->sendOn();
}

void CCSink::send_nack(simtime_picosec ts, CCPacket::seq_t ackno) {
    CCNack *nack = NULL;
    nack = CCNack::newpkt(_src->_flow, *_route, ackno, ts);
    nack->sendOn();
}
