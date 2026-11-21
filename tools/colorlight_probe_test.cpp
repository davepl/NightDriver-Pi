// SPDX-License-Identifier: GPL-3.0-or-later
// Offline request encoding and reply validation; never opens a network device.
#include "tools/colorlight_probe.h"
#include <cassert>
#include <iostream>
using namespace colorlight_probe;
int main() {
 auto discovery = DiscoveryRequest();
 assert(discovery.size()==284 && discovery[12]==7);
 for (size_t i=13;i<discovery.size();++i) assert(discovery[i]==0);
 assert(std::equal(CardMac.begin(),CardMac.end(),discovery.begin()));
 assert(std::equal(SenderMac.begin(),SenderMac.end(),discovery.begin()+6));
 auto read=FlashReadRequest(0x0700);
 assert(read.size()==140 && read[12]==6 && read[13]==0);
 assert(read[14]==0 && read[15]==0 && read[16]==0 && read[17]==0x44 && read[18]==1 && read[19]==7 && read[20]==0);
 for(size_t i=21;i<read.size();++i) assert(read[i]==0);
 assert(!FlashReply(read));
 assert(!DiscoveryReply(discovery)); // own requests must never be accepted
 std::vector<uint8_t> reply(1039,0);
 std::copy(CardMac.begin(),CardMac.end(),reply.begin()+6);
 reply[12]=9;reply[13]=1;reply[14]=0x7f;
 for(size_t i=15;i<reply.size();++i) reply[i]=uint8_t(i);
 assert(FlashReply(reply));
 const auto data=FlashData(reply);
 assert(data.front()==15 && data.back()==uint8_t(1038));
 auto invalid=reply;invalid[6]=0;
 assert(!FlashReply(invalid));
 assert(!FlashReply(std::span(reply).first(1038)));
 auto card=reply;card.resize(1070);card[12]=8;card[13]=5;
 assert(DiscoveryReply(card));
 assert(!DiscoveryReply(std::span(card).first(1069)));
 std::cout << "read-only probe byte/validation tests passed\n";
}
