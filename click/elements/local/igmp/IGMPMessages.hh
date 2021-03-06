#ifndef CLICK_IGMPMESSAGES_H
#define CLICK_IGMPMESSAGES_H

#include <click/element.hh>
#include <cstdint>
#include <click/ipaddress.hh>
#include <algorithm>

inline uint32_t U8toU32(uint8_t byte) {
	if (byte < 128) return byte;

	uint8_t exp  = (byte & 0x70) >> 4;
	uint8_t mant = byte & 0x0F;
	return (mant | 0x10) << (exp + 3);
}

inline uint8_t U32toU8(uint32_t u32) {
	if (u32 < 128) return u32;
	u32 = std::min(u32, 31744u);

	uint32_t index = 31;
	while ((u32 & 1 << index) == 0) { --index; }

	return 0x80 | (((index - 7) & 0x7) << 4) | (u32 >> (index - 4)) & 0xF;
}

// https://tools.ietf.org/html/rfc2113
struct RouterAlertOption {
	// option id
	uint8_t byte1 = 0b10010100;
	uint8_t byte2 = 0b00000100;

	// 0 - Router shall examine packet
	// 1..65536 - Reserved
	uint8_t byte3 = 0b00000000;
	uint8_t byte4 = 0b00000000;
};

enum RecordType : uint8_t {
	/* indicates that the interface has a
	filter mode of INCLUDE for the specified multicast
	address. The Source Address [i] fields in this Group
	Record contain the interface’s source list for the
	specified multicast address, if it is non-empty */
	MODE_IS_INCLUDE = 1,

	/* indicates that the interface has a
	filter mode of EXCLUDE for the specified multicast
	address. The Source Address [i] fields in this Group
	Record contain the interface’s source list for the
	specified multicast address, if it is non-empty */
	MODE_IS_EXCLUDE = 2,

	/* indicates that the interface
	has changed to INCLUDE filter mode for the specified
	multicast address. The Source Address [i] fields
	in this Group Record contain the interface’s new
	source list for the specified multicast address,
	if it is non-empty */
	CHANGE_TO_INCLUDE_MODE = 3,

	/* indicates that the interface
	has changed to EXCLUDE filter mode for the specified
	multicast address. The Source Address [i] fields
	in this Group Record contain the interface’s new
	source list for the specified multicast address,
	if it is non-empty */
	CHANGE_TO_EXCLUDE_MODE = 4,
};

enum MessageType : uint8_t { QUERY = 0x11, REPORT = 0x22 };

struct QueryMessage {
	// Type = 0x11
	MessageType type;

	// 4.1.1. Max Resp Code
	uint8_t maxRespCode;    // uses u8 float

	// 4.1.2. Checksum
	uint16_t checksum;

	// 4.1.3. Group Address
	in_addr groupAddress;

    // 4.1.4. Resv (Reserved)
    // 4.1.5. S Flag (Suppress Router-Side Processing)
    // 4.1.6. QRV (Querier’s Robustness Variable) (max 7)

    // https://en.cppreference.com/w/c/language/bit_field
    // The following properties of bit fields are implementation-defined:
	// The order of bit fields within an allocation unit (on some platforms,
	// bit fields are packed left-to-right, on others right-to-left)
    uint8_t resv_s_qrv;

	// 4.1.7. QQIC (Querier’s Query Interval Code)
	uint8_t qqic;    // uses u8 float

	// 4.1.8. Number of Sources (N)
	uint16_t numSources;

	// 4.1.9. Source Address [i]
	/* The Source Address [i] fields are a vector of n IP unicast addresses,
	where n is the value in the Number of Sources (N) field.*/

	// 4.1.10. Additional Data
	/* If the Packet Length field in the IP header of a received Query
	indicates that there are additional octets of data present, beyond
	the fields described here, IGMPv3 implementations MUST include those
	octets in the computation to verify the received IGMP Checksum, but
	MUST otherwise ignore those additional octets. When sending a Query,
	an IGMPv3 implementation MUST NOT include additional octets beyond
	the fields described here. */

	inline uint32_t maxRespTime() const { return U8toU32(maxRespCode) * 100; }
	uint32_t        QQI() const;
};

struct GroupRecord {
	// 4.2.5. Record Type
	RecordType recordType;

	// 4.2.6. Aux Data Len (units of 32-bit) (must be 0 and ignored)
	uint8_t auxDataLen;

	// 4.2.7. Number of Sources (N)
	uint16_t numSources;

	// 4.2.8. Multicast Address
	in_addr multicastAddress;

	// 4.2.9. Source Address [i]
	/* The Source Address [i] fields are a vector of n IP unicast addresses,
	where n is the value in this record’s Number of Sources (N) field. */

	// 4.2.10. Auxiliary Data
	/* The Auxiliary Data field, if present, contains additional information
	pertaining to this Group Record. The protocol specified in this
	document, IGMPv3, does not define any auxiliary data. Therefore,
	implementations of IGMPv3 MUST NOT include any auxiliary data (i.e.,
	MUST set the Aux Data Len field to zero) in any transmitted Group
	Record, and MUST ignore any auxiliary data present in any received
	Group Record. The semantics and internal encoding of the Auxiliary
	Data field are to be defined by any future version or extension of
	IGMP that uses this field. */
};

struct ReportMessage {
	// Type = 0x22
	MessageType type;

	// 4.2.1. Reserved
	uint8_t reserved;

	// 4.2.2. Checksum
	uint16_t checksum;

	// 4.2.1. Reserved
	uint16_t reserved2;

	// 4.2.3. Number of Group Records (M)
	uint16_t NumGroupRecords;
};

const static uint32_t QRV_DEFAULT = 2;
const static uint32_t QQI_DEFAULT = 125;
const static uint32_t QRI_DEFAULT = 100;

QueryMessage createGeneralQuery();

QueryMessage createGroupSpecificQuery(in_addr groupAddress);

ReportMessage createReportMessage();

GroupRecord createGroupRecord();

#endif    // CLICK_IGMPMESSAGES_H
