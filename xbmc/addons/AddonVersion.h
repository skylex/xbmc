/*
 *      Copyright (C) 2005-2011 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <boost/operators.hpp>
#include "utils/StdString.h"

namespace ADDON
{
  class AddonVersion : public boost::totally_ordered<AddonVersion> {
  public:
    AddonVersion() : mEpoch(0), mUpstream(NULL), mRevision(NULL) {}
    AddonVersion(const AddonVersion& other) { *this = other; }
    explicit AddonVersion(CStdString version);
    ~AddonVersion();

    int Epoch() const { return mEpoch; }
    const char *Upstream() const { return mUpstream; }
    const char *Revision() const { return mRevision; }

    void Epoch(int new_epoch) { mEpoch = new_epoch; }
    void Upstream(const char *new_upstream) { free(mUpstream); mUpstream = strdup(new_upstream); }
    void Revision(const char *new_revision) { free(mRevision); mRevision = strdup(new_revision); }
    void ClearRevision() { Revision("0"); }

    AddonVersion& operator=(const AddonVersion& other);
    bool operator<(const AddonVersion& other) const;
    bool operator==(const AddonVersion& other) const;
    CStdString Print() const;
    const char *c_str() const { return m_originalVersion.c_str(); };
  protected:
    CStdString m_originalVersion;
    int mEpoch;
    char *mUpstream;
    char *mRevision;

    static int CompareComponent(const char *a, const char *b);
  };

  inline AddonVersion::~AddonVersion()
  {
    free(mUpstream);
    free(mRevision);
  }

  inline bool AddonVersion::operator==(const AddonVersion& other) const
  {
    return Epoch() == other.Epoch()
      && strcmp(Upstream(), other.Upstream()) == 0
      && strcmp(Revision(), other.Revision()) == 0;
  }

  inline AddonVersion& AddonVersion::operator=(const AddonVersion& other)
  {
    mEpoch = other.Epoch();
    mUpstream = strdup(other.Upstream());
    mRevision = strdup(other.Revision());
    return *this;
  }
}
