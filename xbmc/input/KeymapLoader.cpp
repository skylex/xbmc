/*
 *      Copyright (C) 2005-2011 Team XBMC
 *      http://xbmc.org
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

#include "KeymapLoader.h"
#include "filesystem/File.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/XMLUtils.h"

using namespace std;
using namespace XFILE;

CKeymapLoader::CKeymapLoader()
{
  if (!parsedMappings)
  {
    ParseDeviceMappings();
  }
}

void CKeymapLoader::DeviceAdded(CStdString deviceId)
{
  CStdString keymapName;
  if (FindMappedDevice(deviceId, keymapName))
  {
    CLog::Log(LOGDEBUG, "Switching Active Keymapping to: %s", keymapName.c_str());
    g_settings.m_activeKeyboardMapping = keymapName;
  }
}

void CKeymapLoader::DeviceRemoved(CStdString deviceId)
{
  CStdString keymapName;
  if (FindMappedDevice(deviceId, keymapName))
  {
    CLog::Log(LOGDEBUG, "Switching Active Keymapping to: default");
    g_settings.m_activeKeyboardMapping = "default";
  }
}

void CKeymapLoader::ParseDeviceMappings()
{
  parsedMappings = true;
  CStdString file("special://xbmc/system/deviceidmappings.xml");
  TiXmlDocument deviceXML;
  if (!CFile::Exists(file) || !deviceXML.LoadFile(file))
    return;

  TiXmlElement *pRootElement = deviceXML.RootElement();
  if (!pRootElement || strcmpi(pRootElement->Value(), "devicemappings") != 0)
    return;
  
  TiXmlElement *pDevice = pRootElement->FirstChildElement("device");
  while (pDevice)
  {
    CStdString deviceId(pDevice->Attribute("id"));
    CStdString keymap(pDevice->Attribute("keymap"));
    if (!deviceId.empty() && !keymap.empty())
      deviceMappings.insert(pair<CStdString, CStdString>(deviceId.ToUpper(), keymap));
    pDevice = pDevice->NextSiblingElement("device");
  }
}

bool CKeymapLoader::FindMappedDevice(CStdString deviceId, CStdString& keymapName)
{
  std::map<CStdString, CStdString>::iterator deviceIdIt = deviceMappings.find(deviceId.ToUpper());
  if (deviceIdIt == deviceMappings.end())
    return false;

  keymapName = deviceIdIt->second;
  return true;
}