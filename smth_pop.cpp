// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include <math.h>
#include <iostream>
#include <algorithm>
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

double _hop_scaling;
double _hops;
double _base_delay;
double _target_delay;
double _alfa;
double _beta;
double _fs_range;
double _fs_min_cwnd;
double _ai;
double _fs_max_cwnd;
double _max_mdf;
double _pacing_delay;
int _retransmit_count;
double _cwnd_prev;
bool _can_decrease;
simtime_picosec _last_decrease;

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
    
    _last_decrease = eventlist.now();
    _alfa = (_fs_range/((1/sqrt(_fs_min_cwnd))-(1/sqrt(_fs_max_cwnd))));
    _beta = 0.5;

    _fs_max_cwnd = _mss * 100;
    _fs_min_cwnd = _mss; 
    _base_delay = 700; // Base target delay in milliseconds (10% of 700ms RTT)
    _ai = 1;
    _target_delay = 1;
    _retransmit_count = 0;
    _hops = 5;
    _hop_scaling = 1;
    _fs_range = 0.5;
    _cwnd = 10 * _mss;
    _max_mdf = 0.5;

    _ssthresh =  (1 - _alfa / 2.0) * _cwnd;
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

bool can_decrease(simtime_picosec now, simtime_picosec rtt){
    return (now - _last_decrease) > rtt;
}

void updateDelay(double _cwnd_aux){
    _target_delay = _target_delay + _base_delay;
    _target_delay = _target_delay + _hop_scaling * _hops;
    _target_delay = _target_delay + max(0.0, min(_alfa/sqrt(_cwnd_aux)+_beta, _fs_range));
}

//Aceasta functie este apelata atunci cand dimensiunea cozii a fost depasita iar packetul cu numarul de secventa ackno a fost aruncat.
void CCSrc::processNack(const CCNack& nack){    
    //cout << "CC " << _name << " got NACK " <<  nack.ackno() << _highest_sent << " at " << timeAsMs(eventlist().now()) << " us" << endl;    
    _nacks_received ++;    
    _flightsize -= _mss;    
    if (nack.ackno()>=_next_decision) {    
        _cwnd = _cwnd / 2;    
        if (_cwnd < _mss)    
            _cwnd = _mss;    
    
        _ssthresh = _cwnd;
            
        //cout << "CWNDD " << _cwnd/_mss << endl; 
        // eventlist.now
    
        _next_decision = _highest_sent + _cwnd;    
    }    
}    
    
/* Process an ACK.  Mostly just housekeeping*/    
void CCSrc::processAck(const CCAck& ack) {    
    CCAck::seq_t ackno = ack.ackno();    
    
    _acks_received++;    
    _flightsize -= _mss;

    /* Swift every RTT update */
    simtime_picosec delay = eventlist().now() - ack.ts();
    simtime_picosec rtt = delay * 2;
    if (can_decrease(eventlist().now(), rtt)){
        updateDelay(_cwnd);
        _last_decrease = eventlist().now();
    }

    if (ack.flags() & ECN_CE && delay > _target_delay){
        //Atunci cand un packet pleaca pe fir, el va fi marcat ECN doar daca dimensiunea cozii este mai mare decat threshold-ul ECN setat. Receptorul va copia acest marcaj in pachetul ACK. Transmitatorul poate lua in calcul reducerea ratei, ca in exemplul mai jos. 
        if (can_decrease(eventlist().now(), rtt)){  
            double a = 1 - _beta * ((delay - _target_delay) / delay);
            double b = 1 - _max_mdf;       
            _cwnd = max(a,b) * _cwnd;
        }
    } else {
        //pachetul nu a fost marcat, putem creste rata.
        if (_cwnd >= 1) {
            _cwnd = _cwnd + _ai / _cwnd * ackno;
        } else {
            _cwnd = _cwnd + _ai * ackno;
        }
    }
    
    //cout << "CWNDI " << _cwnd/_mss << endl;    
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

    p = CCPacket::newpkt(*_route,_flow, _highest_sent+1, _mss, eventlist().now());
    
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

    int size = p->size()-ACKSIZE; 
    _total_received += Packet::data_packet_size();;

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