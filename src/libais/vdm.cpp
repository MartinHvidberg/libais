// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Parse NMEA AIS VDM strings without extra metadata.
//
// TODO(schwehr): Convert the catch (...) blanket exception catches to the
//   expected exception.
// TODO(schwehr): Enable or remove logging messages.

#include "vdm.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>

// #include "base/logging.h"
#include "decode_body.h"
#include "ais.h"

using libais::AisMsg;
using std::ostringstream;
using std::unique_ptr;

namespace libais {

uint8_t Checksum(const string &line) {
  return std::accumulate(line.begin(), line.end(), 0, std::bit_xor<uint8_t>());
}

string ToHex2(int32_t val) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(2) << val;
  return out.str();
}

string ChecksumHexString(const string &base) {
  string checksum = ToHex2(Checksum(base));
  std::transform(checksum.begin(), checksum.end(), checksum.begin(), ::toupper);
  return checksum;
}

vector<string> Split(const string &line, char delim) {
  vector<string> result;
  std::stringstream ss(line);
  while (!ss.eof()) {
    string next;
    std::getline(ss, next, delim);
    result.push_back(next);
  }
  return result;
}

bool ValidateChecksum(const string &line) {
  vector<string> fields = Split(line, "*");

  if (fields.size() != 2 || fields[1].size() != 2) {
    return false;
  }

  try {
    const string checksum_str(fields[1]);
    int32_t checksum = std::stoi(checksum_str, nullptr, 16);

    int32_t checksum_calculated = Checksum(line.substr(1, line.size() - 4));
    if (checksum_calculated != checksum) {
      // LOG(INFO) << "Checksum miss-match: 0x" << ToHex2(checksum_calculated)
      //           << " != 0x" << ToHex2(checksum) << "\n  " << line;
      return false;
    }
  } catch (...) {
    return false;
  }

  return true;
}

string ReportErrorLine(const string &msg, const string &line,
                       int64_t line_number) {
  return "Error on line:" + std::to_string(line_number) + ": " + msg + "\n  " +
         line;
}

bool GetSentenceSequenceNumbers(const string & /* line */,
                                const int64_t /* line_number */,
                                const vector<string> &fields,
                                int32_t *sentence_total,
                                int32_t *sentence_number,
                                int32_t *sequence_number, char *channel) {
  // for (const auto &field : fields) {
  //   std::cerr << "  field: " << field << "\n";
  // }
  try {
    *sentence_total = std::stoi(fields[1]);
  } catch (...) {
    // std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNum\n";
    return false;
  }
  if (*sentence_total < 0 || *sentence_total >= kMaxSentences) {
    // LOG(INFO) << ReportErrorLine("Invalid total", line, line_number);
    std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNumbers\n";
    return false;
  }

  if (fields[2].size() != 1) {
    // std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNumbers\n";
    return false;
  }
  try {
    *sentence_number = std::stoi(fields[2]);
  } catch (...) {
    std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNumbers\n";
    return false;
  }
  if (*sentence_number < 1 || *sentence_number > *sentence_total) {
    // LOG(INFO) << ReportErrorLine("Invalid sentence num", line, line_number);
    // std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNum\n";
    return false;
  }

  if (fields[3].empty()) {
    *sequence_number = -1;
  } else {
    try {
      *sequence_number = std::stoi(fields[3]);
    } catch (...) {
      // std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNum"
      //           << " '" << fields[3] << "'\n";
      return false;
    }
    if (*sequence_number < 0 || *sequence_number >= kNumSequenceChannels) {
      if (fields[3] != "") {
        // std::cerr << __FILE__ << ":" << __LINE__
        //          << " GetSentenceSequenceNumbers\n";
        // LOG(INFO) << ReportErrorLine("Invalid sequence ", line, line_number);
        return false;
      }
      *sequence_number = -1;
    }
  }

  if (fields[4].size() != 1 || (fields[4][0] != 'A' && fields[4][0] != 'B')) {
    // LOG(INFO) << ReportErrorLine("Invalid channel", line, line_number);
    // std::cerr << __FILE__ << ":" << __LINE__ << " GetSentenceSequenceNum\n";
    return false;
  }
  *channel = fields[4][0];

  return true;
}

// TODO(schwehr): Switch to make_unique when C++14 is available on Travis-CI.
template <typename T, typename... Args>
std::unique_ptr<T> MakeUnique(Args &&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// static
unique_ptr<NmeaSentence> NmeaSentence::Create(const string &line,
                                              int64_t line_number) {
  if (line.size() < 9 || line[0] != '!') {
    return nullptr;
  }

  vector<string> fields = Split(line, ",");
  if (fields.size() != 7) {
    // LOG(INFO) << ReportErrorLine("Expected 7 fields", line, line_number);
    // std::cerr << ReportErrorLine("Expected 7 fields", line, line_number);
    return nullptr;
  }

  if (!ValidateChecksum(line)) {
    // LOG(INFO) << ReportErrorLine("Invalid checksum", line, line_number);
    // std::cerr << ReportErrorLine("Invalid checksum", line, line_number);
    return nullptr;
  }

  if (fields[0].size() != 6) {
    // LOG(INFO) << ReportErrorLine("Invalid preamble", line, line_number);
    std::cerr << ReportErrorLine("Invalid preamble", line, line_number);
    return nullptr;
  }

  const string talker(fields[0].substr(1, 2));
  const string sentence_type(fields[0].substr(3));

  int32_t sentence_total;
  int32_t sentence_number;
  int32_t sequence_number;
  char channel;
  if (!GetSentenceSequenceNumbers(line, line_number, fields, &sentence_total,
                                  &sentence_number, &sequence_number,
                                  &channel)) {
    // std::cerr << __FILE__ << ":" << __LINE__ << "\n";
    return nullptr;
  }

  const string body(fields[5]);
  if (body.size() < 1 || body.size() > 199) {
    // LOG(INFO) << ReportErrorLine("Invalid body", line, line_number);
    std::cerr << __FILE__ << ":" << __LINE__ << "\n";
    return nullptr;
  }

  int32_t fill_bits;
  try {
    fill_bits = std::stoi(fields[6].substr(0, 1));
  } catch (...) {
    std::cerr << __FILE__ << ":" << __LINE__ << "\n";
    return nullptr;
  }
  if (fill_bits < 0 || fill_bits > 5) {
    // LOG(INFO) << ReportErrorLine("Invalid fill bits", line, line_number);
    // std::cerr << __FILE__ << ":" << __LINE__ << "\n";
    return nullptr;
  }

  return MakeUnique<NmeaSentence>(talker, sentence_type, sentence_total,
                                  sentence_number, sequence_number, channel,
                                  body, fill_bits, line_number);
}

// TODO(schwehr): Is if faster to just use string.append?
string Join(const vector<string> &parts, char delim) {
  ostringstream out;
  for (const auto &part : parts) {
    if (&part != &parts[0]) {
      out << delim;
    }
  }
  return out.str();
}

unique_ptr<NmeaSentence> NmeaSentence::Merge(
    const vector<unique_ptr<NmeaSentence>> &prior_sentences) const {
  if (prior_sentences.size() != sentence_total_ - 1) {
    // LOG(INFO) << "Prior sentences incomplete.";
    return nullptr;
  }

  // Make sure these sentences really go together.
  for (const auto &prior_sentence : prior_sentences) {
    if (!VerifyInSameMessage(*prior_sentence)) {
      // LOG(INFO) << "Not in the same multi-line message.";
      return nullptr;
    }
  }

  // Check the order.
  for (int i = 0; i < prior_sentences.size(); ++i) {
    if (prior_sentences[i]->sentence_number() != i + 1) {
      // LOG(INFO) << "sentence_number mismatch: "
      //           << prior_sentences[i]->sentence_number() << " != " << i - 1;
      return nullptr;
    }
  }

  string body;
  for (const auto &sentence : prior_sentences) {
    body.append(sentence->body());
  }
  body.append(body_);

  return MakeUnique<NmeaSentence>(talker_, sentence_type_, 1, 1,
                                  sequence_number_, channel_, body, fill_bits_,
                                  line_number_);
}

string NmeaSentence::ToString() const {
  string channel;
  channel += channel_;
  string sequence((sequence_number_ != -1) ? std::to_string(sequence_number_)
                                           : "");

  string result = "";
  result.append(talker_ + sentence_type_);
  result.append(",");
  result.append(std::to_string(sentence_total_));
  result.append(",");
  result.append(std::to_string(sentence_number_));
  result.append(",");
  result.append(sequence);
  result.append(",");
  result.append(channel);
  result.append(",");
  result.append(body_);
  result.append(",");
  result.append(std::to_string(fill_bits_));
  string checksum_str = ChecksumHexString(result);
  result.append("*");
  result.append(checksum_str);
  return "!" + result;
}

string NmeaSentence::ToMd5Digest() const {
  // TODO(schwehr): md5 digest.
  return "";
}

bool NmeaSentence::VerifyInSameMessage(const NmeaSentence &sentence) const {
  if (sentence.talker() != talker_) {
    // LOG(INFO) << "Talker mismatch: " << sentence.talker() << " != "
    //           << talker_;
    return false;
  }
  if (sentence.sentence_type() != sentence_type_) {
    // LOG(INFO) << "sentence_type mismatch: " << sentence.sentence_type()
    //           << " != " << sentence_type_;
    return false;
  }
  if (sentence.sentence_number() == sentence_number_) {
    // LOG(INFO) << "sentence_number must be different.  Both are: "
    //           << sentence.sentence_number();
    return false;
  }
  if (sentence.sentence_total() != sentence_total_) {
    // LOG(INFO) << " mismatch: " << sentence.sentence_total()
    //           << " != " << sentence_total_;
    return false;
  }
  if (sentence.sequence_number() != sequence_number_) {
    // LOG(INFO) << "sequence_number mismatch: " << sentence.sequence_number()
    //           << " != " << sequence_number_;
    return false;
  }
  if (sentence.channel() != channel_) {
    // LOG(INFO) << "Channel mismatch: " << sentence.channel()
    //           << " != " << channel_;
    return false;
  }
  return true;
}

bool VdmStream::AddLine(const string &line) {
  line_number_++;
  auto sentence = NmeaSentence::Create(line, line_number_);
  if (sentence == nullptr) {
    return false;
  }
  int seq = sentence->sequence_number();
  int tot = sentence->sentence_total();

  // These are enforced by NmeaSentence, so only check wehen debugging.
  assert(seq < kNumSequenceChannels);
  assert(tot < kMaxSentences);

  // Convert multi-line message to single line.
  if (tot != 1) {
    int cnt = sentence->sentence_number();

    // LOG(INFO) << "msg: " << seq << " " << tot << " " << cnt;

    // Beginning of a message.
    if (cnt == 1) {
      // LOG_IF(ERROR, !incoming_sentences_[seq].empty())
      //   << "Sequence not empty, but got part 1 of a message. "
      //   << "Dropping old partial.";
      incoming_sentences_[seq].clear();
      incoming_sentences_[seq].emplace_back(std::move(sentence));

      return true;
    }

    // Middle sentences of a message.
    if (cnt != tot) {
      if (incoming_sentences_[seq].size() + 1 != cnt) {
        // LOG(ERROR) << "Out of order part of a sentence. "
        //            << "Dropping incoming sentence. "
        //            << incoming_sentences_[seq].size() + 1 << " != " << cnt;
        return false;
      }
      incoming_sentences_[seq].emplace_back(std::move(sentence));
      return true;
    }

    // Got final sentence in a multi-line message.
    if (incoming_sentences_[seq].size() != tot - 1) {
      // LOG(ERROR) << "Missing parts of a multi-line message. "
      //            << "Dropping all incoming sentence for this seq.  "
      //            << incoming_sentences_[seq].size() << " != " << tot - 1;
      incoming_sentences_[seq].clear();
      return false;
    }

    sentence = sentence->Merge(incoming_sentences_[seq]);
    incoming_sentences_[seq].clear();
    if (sentence == nullptr) {
      // LOG(ERROR) << "Failed to assemble message.";
      return false;
    }

    // Ready to process multi-line message.  Do not return here.
  }

  if (sentence->body().size() < 2) {
    return false;
  }
  unique_ptr<AisMsg> msg =
      CreateAisMsg(sentence->body(), sentence->fill_bits());
  if (msg == nullptr) {
    return false;
  }

  messages_.emplace_front(std::move(msg));
  return true;
}

unique_ptr<AisMsg> VdmStream::PopOldestMessage() {
  if (messages_.empty()) {
    return nullptr;
  }
  unique_ptr<AisMsg> msg(std::move(messages_.back()));
  messages_.pop_back();
  return msg;
}

}  // namespace libais