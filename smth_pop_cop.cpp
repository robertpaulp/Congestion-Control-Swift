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
const double RETX_RESET_THRESHOLD = 300; //TODO: tweak this

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
    _beta = (-_alfa/(sqrt(_fs_max_cwnd)));

    /* Swift varibles */
    _cwnd_prev = 0;
    _min_cwnd = 1 * _mss;
    _max_cwnd = 10000 * _mss;
    _can_decrease = false;
    _t_last_decrease = eventlist.now(); //TODO: check this
    _rtt = 0;
    _retransmit_count = 0;
    _target_delay = 1;
    _ai = 1;                          // Aditive increase
    _betta = 0.5;                     // Multiplicative decrease
    _max_mdf = 0.5;                   // Maximum decrease factor


    _ssthresh =  64 * _mss;
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

void updateDelay(double _cwnd_aux){
    _target_delay = _target_delay + _base_delay;
    _target_delay = _target_delay + _hop_scaling * _hops;
    _target_delay = _target_delay + max(0.0, min(_alfa/sqrt(_cwnd_aux)+_beta, _fs_range));
}
bool can_decrease(simtime_picosec now, simtime_picosec rtt){
    return (timeAsMs(now - _last_decrease) >= rtt);
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

    if (ack.is_ecn_marked()){
        //Atunci cand un packet pleaca pe fir, el va fi marcat ECN doar daca dimensiunea cozii este mai mare decat threshold-ul ECN setat. Receptorul va copia acest marcaj in pachetul ACK. Transmitatorul poate lua in calcul reducerea ratei, ca in exemplul mai jos. 
        if (ackno >=_next_decision){            
            _cwnd = _cwnd / 2;
            if (_cwnd < _mss)
                _cwnd = _mss;
            
            _next_decision = _highest_sent + _cwnd;
        }
    }
    else {
        //pachetul nu a fost marcat, putem creste rata.
        if (_cwnd < _mss)
            //slow start.
            _cwnd += (_mss * 10);    
        else
            //congestion avoidance.
            _cwnd += (_mss*_mss*10 / _cwnd);
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