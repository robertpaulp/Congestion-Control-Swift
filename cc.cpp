#include <math.h>
#include <iostream>
#include <algorithm>
#include <vector> // Added to support vector usage
#include "cc.h"
#include "queue.h"
#include <stdio.h>
#include "switch.h"
#include "ecn.h"

using namespace std;

////////////////////////////////////////////////////////////////
//  CC SOURCE. Aici este codul care ruleaza pe transmitatori. Tot ce avem nevoie pentru a implementa
//  un algoritm de congestion control se gaseste aici.
////////////////////////////////////////////////////////////////
int CCSrc::_global_node_count = 0;

/* Swift Variables */
double _cwnd_prev;
double _min_cwnd;
double _max_cwnd;
bool _can_decrease;
simtime_picosec _t_last_decrease;
double rtt;
uint64_t _retransmit_count;
double _target_delay;
double _ai;
double _betta;
double _max_mdf;
const double RETX_RESET_THRESHOLD = 10;

/* Additional variables */
uint32_t _duplicate_ack;
double _last_ack_no;

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
    
    /* Swift varibles */
    _cwnd_prev = 10 * _mss;
    _min_cwnd = 10 * _mss;
    _max_cwnd = 100000 * _mss;
    _can_decrease = true;
    _t_last_decrease = eventlist.now();
    _retransmit_count = 0;
    _target_delay = 1;
    _ai = 1; // Additive increase
    _betta = 0.5; // Multiplicative decrease
    _max_mdf = 0.5; // Maximum decrease factor

    /* Additional variables */
    _duplicate_ack = 0;
    _last_ack_no = 0;

    _cwnd = 10 * _mss;
    _ssthresh = 0xFFFFFFFFFF;
    _flightsize = 0;
    _flow._name = _nodename;
    setName(_nodename);
}

/* Porneste transmisia fluxului de octeti */
void CCSrc::startflow(){
    cout << "Start flow " << _flow._name << " at " << timeAsSec(eventlist().now()) << "s" << endl;
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

double clamp(double min, double val, double max) {
    return std::max(min, std::min(val, max));
}

//Aceasta functie este apelata atunci cand dimensiunea cozii a fost depasita iar packetul cu numarul de secventa ackno a fost aruncat.
void CCSrc::processNack(const CCNack& nack){    
    //cout << "CC " << _name << " got NACK " <<  nack.ackno() << _highest_sent << " at " << timeAsMs(eventlist().now()) << " us" << endl;    
    _nacks_received ++;    
    _flightsize -= _mss;

    _can_decrease = (eventlist().now() - _t_last_decrease) >= rtt;
    /* On Retransmit Timeout*/
    _retransmit_count++;
    if (_retransmit_count >= RETX_RESET_THRESHOLD) {
        _cwnd = _min_cwnd;
    } else {
        if (_can_decrease) {
            _cwnd = (1 - _max_mdf) * _cwnd;
        }
    }

    /* Enforce lower/upper bounds */
    _cwnd = clamp(_min_cwnd, _cwnd, _max_cwnd);

    if (_cwnd <= _cwnd_prev) {
        _t_last_decrease = eventlist().now();
    }

    _cwnd_prev = _cwnd;
    
}    
    
/* Process an ACK.  Mostly just housekeeping*/    
void CCSrc::processAck(const CCAck& ack) {    
    CCAck::seq_t ackno = ack.ackno();    

    _acks_received++;    
    _flightsize -= _mss;    

    /* Other variables */
    double delay = timeAsMs(eventlist().now()) - timeAsMs(ack.ts());
    simtime_picosec rtt = delay * 2;

    /* Check for duplicate ACKs */
    if (_cwnd/_mss == _last_ack_no) {
        _duplicate_ack++;
    } else {
        _duplicate_ack = 0;
        _last_ack_no = _cwnd/_mss;
    }

    if (_duplicate_ack >= 3) {
        /* Fast Recovery */
        _retransmit_count = 0;
        if (_can_decrease) {
            _cwnd = (1 - _max_mdf) * _cwnd;
        }
    } else {
        /* On Receving ACK */
        _retransmit_count = 0;

        if (delay < _target_delay || !ack.is_ecn_marked()) {
            if (_cwnd >= 1 * _mss) {
                _cwnd = _cwnd + (_ai * _mss *_mss/ _cwnd) * _acks_received;
            } else {
                _cwnd = _cwnd + _ai  * _acks_received * _mss;
            }
        } else {
            if (_can_decrease) {
                double a = 1 - _betta * ((delay - _target_delay) / delay);
                double b = 1 - _max_mdf;
                _cwnd = max(a, b) * _cwnd;
            }
        }
    }

    /* Enforce lower/upper bounds */
    _cwnd = clamp(_min_cwnd, _cwnd, _max_cwnd);

    if (_cwnd <= _cwnd_prev) {
        _t_last_decrease = eventlist().now();
    }

    _cwnd_prev = _cwnd;
    _can_decrease = (eventlist().now() - _t_last_decrease) >= rtt;
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
        cout << "Got packet with type " << pkt.type() << endl;
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

    p = CCPacket::newpkt(*_route, _flow, _highest_sent + 1, _mss, eventlist().now());
    
    _highest_sent += _mss;
    _packets_sent++;

    _flightsize += _mss;

    //cout << "Sent " << _highest_sent+1 << " Flow Size: " << _flow_size << " Flow " << _name << " time " << timeAsUs(eventlist().now()) << endl;
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

        //cout << "Wrong seqno received at CC SINK " << seqno << " expecting " << _cumulative_ack << endl;
        return;
    }

    int size = p->size() - ACKSIZE; 
    _total_received += Packet::data_packet_size();

    bool ecn = (bool)(pkt.flags() & ECN_CE);

    send_ack(ts,seqno,ecn);
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