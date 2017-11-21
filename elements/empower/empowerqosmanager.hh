// -*- mode: c++; c-basic-offset: 2 -*-
#ifndef CLICK_EMPOWERQOSMANAGER_HH
#define CLICK_EMPOWERQOSMANAGER_HH
#include <click/config.h>
#include <click/element.hh>
#include <clicknet/ether.h>
#include <click/notifier.hh>
#include <click/etheraddress.hh>
#include <click/bighashmap.hh>
#include <click/hashmap.hh>
#include <click/hashtable.hh>
#include <click/straccum.hh>
#include <clicknet/wifi.h>
#include <clicknet/llc.h>
#include <elements/standard/simplequeue.hh>
CLICK_DECLS

/*
=c

EmpowerQOSManager(EL, DEBUG)

=s EmPOWER

Converts Ethernet packets to 802.11 packets with a LLC header. Setting
the appropriate BSSID given the destination address. An EmPOWER Access
Point generates one virtual BSSID called LVAP for each active station.
Also maintains a dedicated queue for each pair tenant/dscp.

=d

Strips the Ethernet header off the front of the packet and pushes
an 802.11 frame header and LLC header onto the packet.

Arguments are:

=item EL
An EmpowerLVAPManager element

=item DEBUG
Turn debug on/off

=back 8

=a EmpowerWifiDecap
*/

class AggregationQueue {

public:

	AggregationQueue(uint32_t capacity, EtherAddress ra, EtherAddress ta) {
		_q = new Packet*[capacity];
		_capacity = capacity;
		_ra = ra;
		_ta = ta;
		_size = 0;
		_drops = 0;
		_head = 0;
		_tail = 0;
	}

	String unparse() {
		StringAccum result;
		_queue_lock.acquire_read();
		result << "RA: " << _ra << ", TA: " << _ta << " status: " << _size << "/" << _capacity << "\n";
		_queue_lock.release_read();
		return result.take_string();
	}

	~AggregationQueue() {
		_queue_lock.acquire_write();
		for (uint32_t i = 0; i < _capacity; i++) {
			if (_q[i]) {
				_q[i]->kill();
			}
		}
		delete[] _q;
		_queue_lock.release_write();
	}

	Packet* pull() {
		Packet* p = 0;
		_queue_lock.acquire_write();
		if (_size > 0) {
			p = _q[_head];
			_q[_head] = 0;
			_head++;
			_head %= _capacity;
			_size--;
		}
		_queue_lock.release_write();
		if (p) {
			click_ether *eh = (click_ether *) p->data();
			EtherAddress src = EtherAddress(eh->ether_shost);
			return wifi_encap(p, _ra, src, _ta);
		}
		return 0;
	}

	bool push(Packet* p) {
		bool result = false;
		_queue_lock.acquire_write();
		if (_size == _capacity) {
			_drops++;
			result = false;
		} else {
			_q[_tail] = p;
			_tail++;
			_tail %= _capacity;
			_size++;
			result = true;
		}
		_queue_lock.release_write();
		return result;
	}

    const Packet* top() {
      Packet* p = 0;
      _queue_lock.acquire_write();
      if(_head != _tail) {
        p = _q[(_head+1) % _capacity];
      }
      _queue_lock.release_write();
      return p;
    }

    uint32_t size() { return _size; }

private:

	ReadWriteLock _queue_lock;
	Packet** _q;

	uint32_t _capacity;
	EtherAddress _ra;
	EtherAddress _ta;
	uint32_t _size;
	uint32_t _drops;
	uint32_t _head;
	uint32_t _tail;

    Packet * wifi_encap(Packet *p, EtherAddress ra, EtherAddress sa, EtherAddress ta) {

        WritablePacket *q = p->uniqueify();

		if (!q) {
			return 0;
		}

		uint8_t mode = WIFI_FC1_DIR_FROMDS;
		uint16_t ethtype;

		memcpy(&ethtype, q->data() + 12, 2);

		q->pull(sizeof(struct click_ether));
		q = q->push(sizeof(struct click_llc));

		if (!q) {
			q->kill();
			return 0;
		}

		memcpy(q->data(), WIFI_LLC_HEADER, WIFI_LLC_HEADER_LEN);
		memcpy(q->data() + 6, &ethtype, 2);

		q = q->push(sizeof(struct click_wifi));

		if (!q) {
			q->kill();
			return 0;
		}

		struct click_wifi *w = (struct click_wifi *) q->data();

		memset(q->data(), 0, sizeof(click_wifi));

		w->i_fc[0] = (uint8_t) (WIFI_FC0_VERSION_0 | WIFI_FC0_TYPE_DATA);
		w->i_fc[1] = 0;
		w->i_fc[1] |= (uint8_t) (WIFI_FC1_DIR_MASK & mode);

		memcpy(w->i_addr1, ra.data(), 6);
		memcpy(w->i_addr2, ta.data(), 6);
		memcpy(w->i_addr3, sa.data(), 6);

		return q;

    }
};

typedef HashTable<EtherAddress, AggregationQueue*> AggregationQueues;
typedef AggregationQueues::iterator AQIter;

class TrafficRule {
  public:

    String _ssid;
    int _dscp;

    TrafficRule() : _ssid(""), _dscp(0) {
    }

    TrafficRule(String ssid, int dscp) : _ssid(ssid), _dscp(dscp) {
    }

    inline hashcode_t hashcode() const {
    		return CLICK_NAME(hashcode)(_ssid) + CLICK_NAME(hashcode)(_dscp);
    }

    inline bool operator==(TrafficRule other) const {
    		return (other._ssid == _ssid && other._dscp == _dscp);
    }

    inline bool operator!=(TrafficRule other) const {
    		return (other._ssid != _ssid || other._dscp != _dscp);
    }

	String unparse() {
		StringAccum result;
		result << _ssid << ":" << _dscp;
		return result.take_string();
	}

};

class TrafficRuleQueue {

public:

    AggregationQueues _queues;
	Vector<EtherAddress> _active_list;

	TrafficRule _tr;
    uint32_t _capacity;
    uint32_t _size;
    uint32_t _drops;
    uint32_t _deficit;
    uint32_t _quantum;
    bool _amsdu_aggregation;

	TrafficRuleQueue(TrafficRule tr, uint32_t capacity, uint32_t quantum) :
			_tr(tr), _capacity(capacity), _size(0), _deficit(0),
			_quantum(quantum), _amsdu_aggregation(false), _drops(0) {
	}

	~TrafficRuleQueue() {
	}

    bool enqueue(Packet *p, EtherAddress ra, EtherAddress ta) {

		if (_queues.find(ra) == _queues.end()) {

			click_chatter("%s :: creating new aggregation queue for ra %s ta %s",
					      _tr.unparse().c_str(),
						  ra.unparse().c_str(),
						  ta.unparse().c_str());

			AggregationQueue *queue = new AggregationQueue(_capacity, ra, ta);
			_queues.set(ra, queue);
			_active_list.push_back(ra);

		}

		if (_queues.get(ra)->push(p)) {
			// update size
			_size++;
			// check if ra is in active list
			if (find(_active_list.begin(), _active_list.end(), ra) == _active_list.end()) {
				_active_list.push_back(ra);
			}
			return true;
		}

		_drops++;
		return false;

    }

    Packet *dequeue() {

		if (_active_list.empty()) {
			return 0;
		}

		EtherAddress ra = _active_list[0];
		_active_list.pop_front();

		AQIter active = _queues.find(ra);
		AggregationQueue* queue = active.value();

		Packet *p = queue->pull();

		if (!p) {
			return dequeue();
		}

		_active_list.push_back(ra);
		_size--;
		return p;

    }

	String unparse() {
		StringAccum result;
		result << _tr.unparse() << " -> capacity: " << _capacity << "\n";
		AQIter itr = _queues.begin();
		while (itr != _queues.end()) {
			AggregationQueue *aq = itr.value();
			result << "  " << aq->unparse();
			itr++;
		}
		return result.take_string();
	}

};

typedef HashTable<TrafficRule, TrafficRuleQueue*> TrafficRules;
typedef TrafficRules::iterator TRIter;

typedef HashTable<TrafficRule, Packet*> HeadTable;
typedef HeadTable::iterator HItr;

class EmpowerQOSManager: public SimpleQueue {

public:

	EmpowerQOSManager();
	~EmpowerQOSManager();

	const char *class_name() const { return "EmpowerQOSManager"; }
	const char *port_count() const { return PORTS_1_1; }
	const char *processing() const { return PUSH_TO_PULL; }
    void *cast(const char *);

	int configure(Vector<String> &, ErrorHandler *);

	void push(int, Packet *);
	Packet *pull(int);

	void add_handlers();
	void create_traffic_rule(String, int, int);

	TrafficRules * rules() { return &_rules; }

private:

    enum { SLEEPINESS_TRIGGER = 9 };

    ActiveNotifier _empty_note;
	class EmpowerLVAPManager *_el;
	class Minstrel * _rc;

	TrafficRules _rules;
    HeadTable _head_table;
	Vector<TrafficRule> _active_list;

    int _sleepiness;
    uint32_t _capacity;
    uint32_t _quantum;

    bool _debug;

	void store(String, int, Packet *, EtherAddress, EtherAddress);
	String list_queues();
	uint32_t compute_deficit(Packet *);

	static int write_handler(const String &, Element *, void *, ErrorHandler *);
	static String read_handler(Element *, void *);

};

CLICK_ENDDECLS
#endif
