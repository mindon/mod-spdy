// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MOD_SPDY_HEADER_POPULATOR_INTERFACE_H_
#define MOD_SPDY_HEADER_POPULATOR_INTERFACE_H_

#include <map>
#include <string>

#include "base/basictypes.h"

namespace flip {
typedef std::map<std::string, std::string> FlipHeaderBlock;
}  // namespace flip

namespace mod_spdy {

// Interface for objects which can populate a SPDY header table.
class HeaderPopulatorInterface {
 public:
  HeaderPopulatorInterface() {}
  virtual ~HeaderPopulatorInterface() {}

  // Given an empty header table, populate it.
  virtual void Populate(flip::FlipHeaderBlock* headers) const = 0;

  // Add a header to a header table, merging if necessary.
  static void MergeInHeader(const std::string& key,
                            const std::string& value,
                            flip::FlipHeaderBlock* headers);

 private:
  DISALLOW_COPY_AND_ASSIGN(HeaderPopulatorInterface);
};

}  // namespace mod_spdy

#endif  // MOD_SPDY_HEADER_POPULATOR_INTERFACE_H_