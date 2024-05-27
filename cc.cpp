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
double _target_delay;
double _base_delay;
simtime_picosec _lastDecrease;
double _ai;
double _fsRange;
//double _max_mdfs;
double _hops;
double _hop_scale;
int _retransmit_count;
const int RETX_RESET_THRESHOLD = 3; 
double _pacing_delay;

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
  
    _node_num = _global_node_count++;
    _nodename = "CCsrc " + to_string(_node_num);

    _cwnd = 10 * _mss;
    _max_cwnd = 100000 * _mss;
    _min_cwnd = _mss;
    _cwnd_prev = _cwnd;
    _ai = 0.7;
    //_max_mdfs = -10;
    _fsRange = 2;
    _pacing_delay = 0;
    _base_delay = 1;
    _target_delay = 10;
    _hops = 3;
    _hop_scale = 1;
    _retransmit_count = 0;
    _alpha = _fsRange / ((1/sqrt(_min_cwnd))-(1/sqrt(_max_cwnd)));
    _beta = -1 * _alpha / sqrt(_max_cwnd);
    _lastDecrease = eventlist.now();
 
    _ssthresh = 0xFFFFFFFFFF;
    _flightsize = 0;
    _flow._name = _nodename;
    setName(_nodename);
}

/* Porneste transmisia fluxului de octeti */
void CCSrc::startflow(){
    // cout << "Start flow " << _flow._name << " at " << timeAsSec(eventlist().now()) << "s" << endl;
    _flow_started = true;
    _highest_sent = 0;
    _packets_sent = 0;

    while (_flightsize + _mss < _cwnd)
        send_packet();
}

/* Initializeaza conexiunea la host-ul sink */
void CCSrc::connect(Route* routeout, Route* routeback, CCSink& sink, simtime_picosec starttime) {
    assert(routeout);
    _route = routeout;
    
    _sink = &sink;
    _flow._name = _name;
    _sink->connect(*this, routeback);

    eventlist().sourceIsPending(*this,starttime);
}

/* Variabilele cu care vom lucra:
    _nacks_received
    _flightsize -> numarul de bytes aflati in zbor
    _mss -> maximum segment size
    _next_decision 
    _highest_sent
    _cwnd
    _ssthresh
    
    CCAck._ts -> timestamp ACK
    eventlist.now -> timpul actual
    eventlist.now - CCAck._tx -> latency
    
    ack.ackno();
    
    > Puteti include orice alte variabile in restul codului in functie de nevoie.
*/
/* TODO: In mare parte aici vom avea implementarea algoritmului si in functie de nevoie in celelalte functii */

unordered_map<uint32_t, simtime_picosec> sentTimestamps;

double clamp(double min_val, double val, double max_val) {
    return std::max(min_val, std::min(val, max_val));
}
void updateTargetDelay(double _cwnd_aux) {
    _target_delay = _base_delay + (_hops * _hop_scale);
    _target_delay += std::max(0.0, std::min(1 / sqrt(_cwnd_aux) + _beta, _fsRange));
}

// Aceasta functie este apelata atunci cand dimensiunea cozii a fost depasita iar packetul cu numarul de secventa ackno a fost aruncat.
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
        // cout << "RTT for sequence number " << ackno << ": " << timeAsMs(rtt) << " ms" << endl;
        sentTimestamps.erase(it); // Remove the timestamp entry after calculating RTT
    }

    // Reset retransmission count on receiving a valid ACK
    _retransmit_count = 0;
    updateTargetDelay(_cwnd);
    if (ack.is_ecn_marked()) {
        // Handle ECN marked ACK
        if (currentTimestamp - _lastDecrease >= timeAsMs(rtt)) {
            _cwnd *= (1.0 - _beta * (timeAsMs(rtt))/_target_delay); // Multiplicative decrease
            _lastDecrease = currentTimestamp;
        }
    } else {
        // Increase the congestion window
        if (_cwnd < _ssthresh) {
            // Slow start phase
            _cwnd += _mss*_ai;    
        } else {
            // Congestion avoidance phase
            _cwnd += (_ai * _mss) / _cwnd;
        }
         _next_decision = _highest_sent + _cwnd;  
    }

    // Ensure the congestion window stays within bounds
    _cwnd = clamp(_min_cwnd, _cwnd, _max_cwnd);
    if (_cwnd <= _cwnd_prev) {
        _lastDecrease = currentTimestamp;
    }

    // Calculate pacing delay
    _pacing_delay = timeAsMs(rtt) / _cwnd;

    // Debugging information
    // cout << "ACK processed: _cwnd=" << _cwnd << ", _pacing_delay=" << _pacing_delay << endl;

    // Additional logic as needed
}

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
        // cout << "RTT for sequence number " << nack.ackno() << ": " << timeAsMs(rtt) << " ms" << endl;
        sentTimestamps.erase(it); // Remove the timestamp entry after calculating RTT
    }

    // On Retransmit Timeout
    _retransmit_count++;
    if (_retransmit_count >= RETX_RESET_THRESHOLD) {
        _cwnd = _min_cwnd; // Drastic reduction for repeated timeouts
    } else {
        _cwnd *= (1.0 - _beta); // Multiplicative decrease
        _ssthresh = _cwnd;
        _next_decision = _highest_sent + _cwnd;  

    }

    // Ensure the congestion window stays within bounds
    _cwnd = clamp(_min_cwnd, _cwnd, _max_cwnd);
    if (_cwnd <= _cwnd_prev) {
        _lastDecrease = currentTimestamp;
    }

    // Calculate pacing delay
    _pacing_delay = timeAsMs(rtt) / _cwnd;

    // Debugging information
    // cout << "NACK processed: _cwnd=" << _cwnd << ", _pacing_delay=" << _pacing_delay << endl;

    // Additional logic as needed
}


/* Functia de receptie, in functie de ce primeste cheama processLoss sau processACK */
void CCSrc::receivePacket(Packet& pkt) 
{
    if (!_flow_started){
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
        // cout << "Got packet with type " << pkt.type() << endl;
        abort();
    }

    //now send packets!
    while (_flightsize + _mss < _cwnd)
        send_packet();
}

// Note: the data sequence number is the number of Byte1 of the packet, not the last byte.
/* Functia care se este chemata pentru transmisia unui pachet */
void CCSrc::send_packet() {
    CCPacket* p = NULL;

    assert(_flow_started);
    simtime_picosec currentTimestamp = eventlist().now();
    p = CCPacket::newpkt(*_route,_flow, _highest_sent+1, _mss, currentTimestamp);
    sentTimestamps[_highest_sent + 1] = currentTimestamp;

    _highest_sent += _mss;
    _packets_sent++;

    _flightsize += _mss;

    // Debugging information
    // cout << "Packet sent: _highest_sent=" << _highest_sent << ", _flightsize=" << _flightsize << endl;

    //// cout << "Sent " << _highest_sent+1 << " Flow Size: " << _flow_size << " Flow " << _name << " time " << timeAsUs(eventlist().now()) << endl;
    p->sendOn();
}

void CCSrc::doNextEvent() {
    if (!_flow_started){
      startflow();
      return;
    }
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

    if (pkt.header_only()){
        send_nack(ts,seqno);      
    
        p->free();

        //// cout << "Wrong seqno received at CC SINK " << seqno << " expecting " << _cumulative_ack << endl;
        return;
    }

    int size = p->size()-ACKSIZE; 
    _total_received += Packet::data_packet_size();

    bool ecn = (bool)(pkt.flags() & ECN_CE);

    send_ack(ts,seqno,ecn);
    // have we seen everything yet?
    pkt.free();
}

void CCSink::send_ack(simtime_picosec ts,CCPacket::seq_t ackno,bool ecn) {
    CCAck *ack = 0;
    ack = CCAck::newpkt(_src->_flow, *_route, ackno,ts,ecn);
    ack->sendOn();
}

void CCSink::send_nack(simtime_picosec ts, CCPacket::seq_t ackno) {
    CCNack *nack = NULL;
    nack = CCNack::newpkt(_src->_flow, *_route, ackno,ts);
    nack->sendOn();
}