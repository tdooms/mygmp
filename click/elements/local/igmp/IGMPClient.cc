#include <click/config.h>
#include <click/args.hh>
#include <click/error.hh>
#include <click/timer.hh>
#include <clicknet/ether.h>
#include <string>
#include "IGMPClient.hh"

CLICK_DECLS
/**
 * read the reference to the IGMPClientState
 * @param conf
 * @param errh
 * @return
 */
int IGMPClient::configure(Vector<String>& conf, ErrorHandler* errh) {
	if (Args(conf, this, errh)
	        .read_mp("STATE", ElementCastArg("IGMPClientState"), state)
	        .complete()) {
		return errh->error("Could not parse IGMPClientState");
	}

	generalTimer = new Timer(&handleGeneralReport, (void*) this);
	generalTimer->initialize(this);

	return 0;
}

/**
 * register handlers
 */
void IGMPClient::add_handlers() {
	add_write_handler("join", &handleJoin, nullptr);
	add_write_handler("leave", &handleLeave, nullptr);
}

/**
 * schedule general/group report in response to a query
 * @param p
 */
void IGMPClient::push(int, Packet* p) {
	RouterAlertOption option{};
	if (!(p->ip_header_length() > 5 * 4 &&
	      !memcmp((p->data() + p->ip_header_length() - 4), &option, sizeof(RouterAlertOption)))) {
		p->kill();
		click_chatter("Dropped packet without alert option");
		return;
	}

	auto query = (QueryMessage*) (((const unsigned char*) (((click_ip*) p->data()) + 1)) + 4);

	if (query->type != QUERY) {
		p->kill();
		return;
	}

	if (click_in_cksum((const unsigned char*) query, sizeof(QueryMessage))) {
		p->kill();
		click_chatter("Dropped wrong checksum packet.");
		return;
	}

	unsigned char new_qrv = query->resv_s_qrv & 0x7;
	qrv = new_qrv ? new_qrv : 2u;

	//	if (!state->hasState()) return;
	auto delay = (int) (((float) rand() / (float) RAND_MAX) * query->maxRespTime());

	if (generalTimer->scheduled() &&
	    (generalTimer->expiry_steady() - Timestamp::now_steady()).msecval() < delay) {
		return;
	} else if (query->groupAddress == 0) {
		generalTimer->schedule_after_msec(delay);
	} else if (!groupTimers.count(query->groupAddress)) {
		auto report = new ScheduledGroupReport{ this, query->groupAddress };
		auto timer  = new Timer(&handleGroupReport, (void*) report);
		timer->initialize(this);
		timer->schedule_after_msec(delay);
		groupTimers[query->groupAddress] = timer;
	} else if ((groupTimers[query->groupAddress]->expiry_steady() - Timestamp::now_steady())
	               .msecval() > delay) {
		groupTimers[query->groupAddress]->schedule_after_msec(delay);
	}
}

/**
 * handler for the join command
 * @param conf
 * @param e
 * @param thunk
 * @param errh
 * @return
 */
int IGMPClient::handleJoin(const String& conf, Element* e, void* thunk, ErrorHandler* errh) {
	auto client = (IGMPClient*) e;

	Vector<String> vconf;
	cp_argvec(conf, vconf);

	IPAddress address;

	if (Args(vconf, client, errh).read_mp("ADDRESS", address).complete() < 0) {
		return errh->error("Could not parse multicast-address");
	}

	if (client->state->addAddress(address)) {
		client->scheduleStateChangeMessage(CHANGE_TO_EXCLUDE_MODE, address);
	}

	return 0;
}

/**
 * handler for the leave command
 * @param conf
 * @param e
 * @param thunk
 * @param errh
 * @return
 */
int IGMPClient::handleLeave(const String& conf, Element* e, void* thunk, ErrorHandler* errh) {
	auto client = (IGMPClient*) e;

	Vector<String> vconf;
	cp_argvec(conf, vconf);

	IPAddress address;

	if (Args(vconf, client, errh).read_mp("ADDRESS", address).complete() < 0) {
		return errh->error("Could not parse multicast-address");
	}

	if (client->state->removeAddress(address)) {
		client->scheduleStateChangeMessage(CHANGE_TO_INCLUDE_MODE, address);
	}

	return 0;
}

/**
 * schedule an unsolicited interface change report
 * @param type type of the record
 * @param address groupaddress
 */
void IGMPClient::scheduleStateChangeMessage(RecordType type, IPAddress address) {
	auto packet = Packet::make(sizeof(click_ether) + sizeof(click_ip), 0,
	                           sizeof(ReportMessage) + sizeof(GroupRecord), 0);

	if (!packet) {
		click_chatter("Could not allocate packet");
		return;
	}
	memset(packet->data(), 0, packet->length());

	auto header             = (ReportMessage*) packet->data();
	header->type            = REPORT;
	header->NumGroupRecords = htons(1);

	auto record              = (GroupRecord*) (header + 1);
	record->recordType       = type;
	record->multicastAddress = address.in_addr();

	header->checksum =
		click_in_cksum((const unsigned char*) header, sizeof(ReportMessage) + sizeof(GroupRecord));

	output(0).push(packet->clone());
	printMessage("Interface Change: " + std::to_string(qrv - 1) + " remaining", header);

	auto iter = changeTimers.find(address);
	if (iter != changeTimers.end()) {
		(*iter).second->clear();
		delete (*iter).second;
	}

	if (qrv <= 1) return;

	auto timerdata = new ScheduledChangeReport{ this, packet, qrv - 1 };
	auto timer     = new Timer(&handleChangeReport, timerdata);
	timer->initialize(this);
	timer->schedule_after_msec((float) rand() / (float) RAND_MAX * unsolicitedReportInterval);
	changeTimers[address] = timer;
}

/**
 * send an unsolicited interface change report
 * @param timer
 * @param data ScheduledChangeReport
 */
void IGMPClient::handleChangeReport(Timer* timer, void* data) {
	auto* report = (ScheduledChangeReport*) data;
	assert(report);
	report->client->output(0).push(report->packet->clone());
	printMessage("Interface Change: " + std::to_string(report->remaining - 1) + " remaining",
	             (ReportMessage*) report->packet->data());
	if (--report->remaining <= 0) {
		timer->clear();
		return;
	}
	timer->schedule_after_msec((float) rand() / (float) RAND_MAX *
	                           report->client->unsolicitedReportInterval);
}

/**
 * send a general report message
 * @param timer to expire
 * @param data IGMPClient
 */
void IGMPClient::handleGeneralReport(Timer* timer, void* data) {
	auto client = (IGMPClient*) data;
	assert(client);
	timer->clear();

	if (!client->state->hasState()) return;

	auto packet =
		Packet::make(sizeof(click_ether) + sizeof(click_ip), 0,
	                 sizeof(ReportMessage) + sizeof(GroupRecord) * client->state->size(), 0);
	if (!packet) {
		click_chatter("Could not allocate packet");
		return;
	}
	memset(packet->data(), 0, packet->length());

	auto header             = (ReportMessage*) packet->data();
	header->type            = REPORT;
	header->NumGroupRecords = htons(client->state->size());

	auto record = (GroupRecord*) (header + 1);
	for (const auto& address : *client->state) {
		record->recordType       = MODE_IS_EXCLUDE;
		record->multicastAddress = address.in_addr();
		record++;
	}

	header->checksum =
		click_in_cksum((const unsigned char*) header,
	                   sizeof(ReportMessage) + sizeof(GroupRecord) * client->state->size());
	client->output(0).push(packet);
	printMessage("General", header);
}

/**
 * send a Group specific report message
 * @param timer that expires
 * @param data ScheduledGroupReport
 */
void IGMPClient::handleGroupReport(Timer* timer, void* data) {
	auto report = (ScheduledGroupReport*) data;
	assert(report);

	timer->clear();
	delete timer;
	report->client->groupTimers.erase(report->address);

	if (!report->client->state->hasAddress(report->address) ||
	    report->address == IPAddress("224.0.0.1"))
		return;

	auto packet = Packet::make(sizeof(click_ether) + sizeof(click_ip), 0,
	                           sizeof(ReportMessage) + sizeof(GroupRecord), 0);
	if (!packet) {
		click_chatter("Could not allocate packet");
		return;
	}
	memset(packet->data(), 0, packet->length());

	auto header             = (ReportMessage*) packet->data();
	header->type            = REPORT;
	header->NumGroupRecords = htons(1);

	auto record              = (GroupRecord*) (header + 1);
	record->recordType       = MODE_IS_EXCLUDE;
	record->multicastAddress = report->address;

	header->checksum =
		click_in_cksum((const unsigned char*) header, sizeof(ReportMessage) + sizeof(GroupRecord));
	report->client->output(0).push(packet);
	printMessage("Group", header);
}

/**
 * print the content of a report message
 * @param front text to put in front
 * @param message
 */
void printMessage(std::string front, ReportMessage* message) {
	click_chatter("%s:\treport", front.c_str());
	for (uint16_t i = 0; i < ntohs(message->NumGroupRecords); ++i) {
		auto        record = (GroupRecord*) (message + 1) + i;
		std::string type;
		switch (record->recordType) {
		case MODE_IS_INCLUDE: type = "is_inc"; break;
		case MODE_IS_EXCLUDE: type = "is_exc"; break;
		case CHANGE_TO_INCLUDE_MODE: type = "to_inc"; break;
		case CHANGE_TO_EXCLUDE_MODE: type = "to_exc"; break;
		}
		click_chatter("\t%s %s", type.c_str(),
		              IPAddress(record->multicastAddress).unparse().c_str());
	}
}

CLICK_ENDDECLS
EXPORT_ELEMENT(IGMPClient)
