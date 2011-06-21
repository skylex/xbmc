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

#include "Application.h"
#include "GUIInfoManager.h"
#ifdef HAS_VIDEO_PLAYBACK
#include "cores/VideoRenderers/RenderManager.h"
#endif

#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogProgress.h"
#include "dialogs/GUIDialogBusy.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "music/tags/MusicInfoTag.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "windows/GUIWindowPVR.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"

#include "PVRManager.h"
#include "PVRDatabase.h"
#include "PVRGUIInfo.h"
#include "addons/PVRClients.h"
#include "channels/PVRChannelGroupsContainer.h"
#include "epg/PVREpgContainer.h"
#include "recordings/PVRRecordings.h"
#include "timers/PVRTimers.h"

using namespace std;
using namespace MUSIC_INFO;
using namespace PVR;

CPVRManager::CPVRManager(void) :
    m_channelGroups(NULL),
    m_epg(NULL),
    m_recordings(NULL),
    m_timers(NULL),
    m_addons(NULL),
    m_guiInfo(NULL),
    m_currentFile(NULL),
    m_database(NULL),
    m_bFirstStart(true),
    m_bLoaded(false),
    m_loadingBusyDialog(NULL),
    m_currentRadioGroup(NULL),
    m_currentTVGroup(NULL)
{
  ResetProperties();
}

CPVRManager::~CPVRManager(void)
{
  Stop();
  Cleanup();

  CLog::Log(LOGDEBUG,"PVRManager - destroyed");
}

void CPVRManager::Notify(const Observable &obs, const CStdString& msg)
{
  // TODO process notifications from the EPG
}

CPVRManager &CPVRManager::Get(void)
{
  static CPVRManager pvrManagerInstance;
  return pvrManagerInstance;
}

void CPVRManager::Start(void)
{
  /* first stop and remove any clients */
  Stop();

  /* don't start if Settings->Video->TV->Enable isn't checked */
  if (!g_guiSettings.GetBool("pvrmanager.enabled"))
    return;

  ResetProperties();

  /* create the supervisor thread to do all background activities */
  StartUpdateThreads();
}

void CPVRManager::Stop(void)
{
  /* check whether the pvrmanager is loaded */
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return;
  m_bLoaded = false;
  lock.Leave();

  CLog::Log(LOGNOTICE, "PVRManager - stopping");

  /* stop playback if needed */
  if (IsPlaying())
  {
    CLog::Log(LOGNOTICE,"PVRManager - %s - stopping PVR playback", __FUNCTION__);
    g_application.StopPlaying();
  }

  /* stop all update threads */
  StopUpdateThreads();

  /* unload all data */
  m_epg->UnregisterObserver(this);

  m_recordings->Unload();
  m_timers->Unload();
  m_epg->Unload();
  m_channelGroups->Unload();
  m_addons->Unload();
}

bool CPVRManager::StartUpdateThreads(void)
{
  StopUpdateThreads();
  CLog::Log(LOGNOTICE, "PVRManager - starting up");

  /* create the pvrmanager thread, which will ensure that all data will be loaded */
  Create();
  SetName("XBMC PVRManager");
  SetPriority(-1);

  return true;
}

void CPVRManager::StopUpdateThreads(void)
{
  StopThread();
  m_epg->UnregisterObserver(this);
  m_epg->Stop();
  m_guiInfo->Stop();
  m_addons->Stop();
}

void CPVRManager::Cleanup(void)
{
  delete m_guiInfo;            m_guiInfo = NULL;
  delete m_timers;             m_timers = NULL;
  delete m_epg;                m_epg = NULL;
  delete m_recordings;         m_recordings = NULL;
  delete m_channelGroups;      m_channelGroups = NULL;
  delete m_addons;             m_addons = NULL;
  delete m_database;           m_database = NULL;
  CloseHandle(m_triggerEvent); m_triggerEvent = NULL;
}

bool CPVRManager::Load(void)
{
  /* load at least one client */
  while (!m_addons->HasActiveClients() && !m_bStop)
    m_addons->TryLoadClients(1);

  if (m_addons->HasActiveClients() && !m_bLoaded && !m_bStop)
  {
    CLog::Log(LOGDEBUG, "PVRManager - %s - active clients found. continue to start", __FUNCTION__);
    ShowBusyDialog(true);

    /* load all channels and groups */
    if (!m_bStop)
      m_channelGroups->Load();

    /* get timers from the backends */
    if (!m_bStop)
      m_timers->Load();

    /* get recordings from the backend */
    if (!m_bStop)
      m_recordings->Load();

    ShowBusyDialog(false);
    m_bLoaded = true;
  }

  return m_bLoaded;
}

void CPVRManager::ShowBusyDialog(bool bShow)
{
  if (bShow)
  {
    /* show the dialog */
    if (!m_loadingBusyDialog)
      m_loadingBusyDialog = (CGUIDialogBusy*)g_windowManager.GetWindow(WINDOW_DIALOG_BUSY);
    m_loadingBusyDialog->Show();
  }
  else if (m_loadingBusyDialog && m_loadingBusyDialog->IsActive())
  {
    /* hide the dialog */
    m_loadingBusyDialog->Close();
    m_loadingBusyDialog = NULL;
  }
}

void CPVRManager::Process(void)
{
  /* load the pvr data from the db and clients if it's not already loaded */
  if (!Load())
  {
    CLog::Log(LOGERROR, "PVRManager - %s - failed to load PVR data", __FUNCTION__);
    return;
  }

  /* reset observers that are observing pvr related data in the pvr windows, or updates won't work after a reload */
  CGUIWindowPVR *pWindow = (CGUIWindowPVR *) g_windowManager.GetWindow(WINDOW_PVR);
  if (pWindow)
    pWindow->Reset();

  /* start the other pvr related update threads */
  m_addons->Start();
  m_guiInfo->Start();
  m_epg->RegisterObserver(this);
  m_epg->Start();

  /* continue last watched channel after first startup */
  if (!m_bStop && m_bFirstStart && g_guiSettings.GetInt("pvrplayback.startlast") != START_LAST_CHANNEL_OFF)
    ContinueLastChannel();

  /* close the busy dialog */
  ShowBusyDialog(false);

  /* signal to window that clients are loaded */
  if (pWindow)
    pWindow->UnlockWindow();

  /* check whether all channel icons are cached */
  m_channelGroups->GetGroupAllRadio()->CacheIcons();
  m_channelGroups->GetGroupAllTV()->CacheIcons();

  CLog::Log(LOGDEBUG, "PVRManager - %s - entering main loop", __FUNCTION__);

  /* main loop */
  while (!m_bStop)
  {
    /* keep trying to load remaining clients if they're not already loaded */
    if (!m_addons->AllClientsLoaded())
      m_addons->TryLoadClients(1);

    /* execute the next pending jobs if there are any */
    if (m_addons->HasActiveClients())
      ExecutePendingJobs();

    /* check if the (still) are any enabled addons */
    if (!m_addons->HasActiveClients())
    {
      CLog::Log(LOGNOTICE, "PVRManager - %s - no add-ons enabled anymore. restarting the pvrmanager", __FUNCTION__);
      Stop();
      Start();
      return;
    }

    WaitForSingleObject(m_triggerEvent, 1000);
  }

}

bool CPVRManager::ChannelSwitch(unsigned int iChannelNumber)
{
  CSingleLock lock(m_critSection);

  const CPVRChannelGroup *playingGroup = GetPlayingGroup(m_addons->IsPlayingRadio());
  if (playingGroup == NULL)
  {
    CLog::Log(LOGERROR, "PVRManager - %s - cannot get the selected group", __FUNCTION__);
    return false;
  }

  const CPVRChannel *channel = playingGroup->GetByChannelNumber(iChannelNumber);
  if (channel == NULL)
  {
    CLog::Log(LOGERROR, "PVRManager - %s - cannot find channel %d", __FUNCTION__, iChannelNumber);
    return false;
  }

  return PerformChannelSwitch(*channel, false);
}

bool CPVRManager::ChannelUpDown(unsigned int *iNewChannelNumber, bool bPreview, bool bUp)
{
  bool bReturn = false;
  if (IsPlayingTV() || IsPlayingRadio())
  {
    CFileItem currentFile(g_application.CurrentFileItem());
    CPVRChannel *currentChannel = currentFile.GetPVRChannelInfoTag();
    const CPVRChannelGroup *group = GetPlayingGroup(currentChannel->IsRadio());
    if (group)
    {
      const CPVRChannel *newChannel = bUp ? group->GetByChannelUp(*currentChannel) : group->GetByChannelDown(*currentChannel);
      if (PerformChannelSwitch(*newChannel, bPreview))
      {
        *iNewChannelNumber = newChannel->ChannelNumber();
        bReturn = true;
      }
    }
  }

  return bReturn;
}

bool CPVRManager::ContinueLastChannel(void)
{
  CSingleLock lock(m_critSection);
  if (!m_bFirstStart)
    return true;
  m_bFirstStart = false;
  lock.Leave();

  bool bReturn(false);
  const CPVRChannel *channel = m_channelGroups->GetLastPlayedChannel();
  if (channel != NULL)
  {
    CLog::Log(LOGNOTICE, "PVRManager - %s - continue playback on channel '%s'",
        __FUNCTION__, channel->ChannelName().c_str());
    bReturn = StartPlayback(channel, (g_guiSettings.GetInt("pvrplayback.startlast") == START_LAST_CHANNEL_MIN));
  }

  return bReturn;
}

void CPVRManager::ResetProperties(void)
{
  if (!m_triggerEvent)  m_triggerEvent  = CreateEvent(NULL, TRUE, TRUE, NULL);
  if (!m_database)      m_database      = new CPVRDatabase;
  if (!m_addons)        m_addons        = new CPVRClients;
  if (!m_channelGroups) m_channelGroups = new CPVRChannelGroupsContainer;
  if (!m_epg)           m_epg           = new CPVREpgContainer;
  if (!m_recordings)    m_recordings    = new CPVRRecordings;
  if (!m_timers)        m_timers        = new CPVRTimers;
  if (!m_guiInfo)       m_guiInfo       = new CPVRGUIInfo;

  m_currentFile           = NULL;
  m_currentRadioGroup     = NULL;
  m_currentTVGroup        = NULL;
  m_PreviousChannel[0]    = -1;
  m_PreviousChannel[1]    = -1;
  m_PreviousChannelIndex  = 0;
  m_LastChannel           = 0;

  for (unsigned int iJobPtr = 0; iJobPtr < m_pendingUpdates.size(); iJobPtr++)
    delete m_pendingUpdates.at(iJobPtr);
  m_pendingUpdates.clear();
}

void CPVRManager::ResetDatabase(bool bShowProgress /* = true */)
{
  CLog::Log(LOGNOTICE,"PVRManager - %s - clearing the PVR database", __FUNCTION__);

  /* close the epg progress dialog, or we'll get a deadlock */
  g_PVREpg->CloseUpdateDialog();

  CGUIDialogProgress* pDlgProgress = NULL;
  if (bShowProgress)
  {
    pDlgProgress = (CGUIDialogProgress*)g_windowManager.GetWindow(WINDOW_DIALOG_PROGRESS);
    pDlgProgress->SetLine(0, "");
    pDlgProgress->SetLine(1, g_localizeStrings.Get(19186));
    pDlgProgress->SetLine(2, "");
    pDlgProgress->StartModal();
    pDlgProgress->Progress();
  }

  if (m_addons->IsPlaying())
  {
    CLog::Log(LOGNOTICE,"PVRManager - %s - stopping playback", __FUNCTION__);
    g_application.StopPlaying();
  }

  if (bShowProgress)
  {
    pDlgProgress->SetPercentage(10);
    pDlgProgress->Progress();
  }

  /* stop the thread */
  if (g_guiSettings.GetBool("pvrmanager.enabled"))
    Stop();

  if (bShowProgress)
  {
    pDlgProgress->SetPercentage(20);
    pDlgProgress->Progress();
  }

  if (m_database->Open())
  {
    /* clean the EPG database */
    m_epg->Clear(true);
    if (bShowProgress)
    {
      pDlgProgress->SetPercentage(30);
      pDlgProgress->Progress();
    }

    m_database->DeleteChannelGroups();
    if (bShowProgress)
    {
      pDlgProgress->SetPercentage(50);
      pDlgProgress->Progress();
    }

    /* delete all channels */
    m_database->DeleteChannels();
    if (bShowProgress)
    {
      pDlgProgress->SetPercentage(70);
      pDlgProgress->Progress();
    }

    /* delete all channel settings */
    m_database->DeleteChannelSettings();
    if (bShowProgress)
    {
      pDlgProgress->SetPercentage(80);
      pDlgProgress->Progress();
    }

    /* delete all client information */
    m_database->DeleteClients();
    if (bShowProgress)
    {
      pDlgProgress->SetPercentage(90);
      pDlgProgress->Progress();
    }

    m_database->Close();
  }

  CLog::Log(LOGNOTICE,"PVRManager - %s - PVR database cleared", __FUNCTION__);

  if (g_guiSettings.GetBool("pvrmanager.enabled"))
  {
    CLog::Log(LOGNOTICE,"PVRManager - %s - restarting the PVRManager", __FUNCTION__);
    Cleanup();
    Start();
  }

  if (bShowProgress)
  {
    pDlgProgress->SetPercentage(100);
    pDlgProgress->Close();
  }
}

void CPVRManager::ResetEPG(void)
{
  CLog::Log(LOGNOTICE,"PVRManager - %s - clearing the EPG database", __FUNCTION__);

  StopUpdateThreads();
  m_epg->Reset();

  if (g_guiSettings.GetBool("pvrmanager.enabled"))
    StartUpdateThreads();
}

bool CPVRManager::IsPlaying(void) const
{
  return m_bLoaded && m_addons->IsPlaying();
}

bool CPVRManager::GetCurrentChannel(CPVRChannel *channel) const
{
  return m_addons->GetPlayingChannel(channel);
}

int CPVRManager::GetCurrentEpg(CFileItemList *results) const
{
  int iReturn = -1;

  CPVRChannel channel;
  if (m_addons->GetPlayingChannel(&channel))
    iReturn = channel.GetEPG(results);
  else
    CLog::Log(LOGDEBUG,"PVRManager - %s - no current channel set", __FUNCTION__);

  return iReturn;
}

int CPVRManager::GetPreviousChannel(void)
{
  //XXX this must be the craziest way to store the last channel
  int iReturn = -1;
  CPVRChannel channel;
  if (m_addons->GetPlayingChannel(&channel))
  {
    int iLastChannel = channel.ChannelNumber();

    if ((m_PreviousChannel[m_PreviousChannelIndex ^ 1] == iLastChannel || iLastChannel != m_PreviousChannel[0]) &&
        iLastChannel != m_PreviousChannel[1])
      m_PreviousChannelIndex ^= 1;

    iReturn = m_PreviousChannel[m_PreviousChannelIndex ^= 1];
  }

  return iReturn;
}

bool CPVRManager::StartRecordingOnPlayingChannel(bool bOnOff)
{
  bool bReturn = false;

  CPVRChannel channel;
  if (!m_addons->GetPlayingChannel(&channel))
    return bReturn;

  if (m_addons->HasTimerSupport(channel.ClientID()))
  {
    /* timers are supported on this channel */
    if (bOnOff && !channel.IsRecording())
    {
      CPVRTimerInfoTag *newTimer = m_timers->InstantTimer(&channel);
      if (!newTimer)
        CGUIDialogOK::ShowAndGetInput(19033,0,19164,0);
      else
        bReturn = true;
    }
    else if (!bOnOff && channel.IsRecording())
    {
      /* delete active timers */
      bReturn = m_timers->DeleteTimersOnChannel(channel, false, true);
    }
  }

  return bReturn;
}

void CPVRManager::SaveCurrentChannelSettings(void)
{
  CSingleLock lock(m_critSection);

  CPVRChannel channel;
  if (!m_addons->GetPlayingChannel(&channel))
    return;

  if (!m_database->Open())
  {
    CLog::Log(LOGERROR, "PVR - %s - could not open the database", __FUNCTION__);
    return;
  }

  if (g_settings.m_currentVideoSettings != g_settings.m_defaultVideoSettings)
  {
    CLog::Log(LOGDEBUG, "PVR - %s - persisting custom channel settings for channel '%s'",
        __FUNCTION__, channel.ChannelName().c_str());
    m_database->PersistChannelSettings(channel, g_settings.m_currentVideoSettings);
  }
  else
  {
    CLog::Log(LOGDEBUG, "PVR - %s - no custom channel settings for channel '%s'",
        __FUNCTION__, channel.ChannelName().c_str());
    m_database->DeleteChannelSettings(channel);
  }

  m_database->Close();
}

void CPVRManager::LoadCurrentChannelSettings()
{
  CPVRChannel channel;
  if (!m_addons->GetPlayingChannel(&channel))
    return;

  if (!m_database->Open())
  {
    CLog::Log(LOGERROR, "PVR - %s - could not open the database", __FUNCTION__);
    return;
  }

  if (g_application.m_pPlayer)
  {
    /* set the default settings first */
    CVideoSettings loadedChannelSettings = g_settings.m_defaultVideoSettings;

    /* try to load the settings from the database */
    m_database->GetChannelSettings(channel, loadedChannelSettings);
    m_database->Close();

    g_settings.m_currentVideoSettings = g_settings.m_defaultVideoSettings;
    g_settings.m_currentVideoSettings.m_Brightness          = loadedChannelSettings.m_Brightness;
    g_settings.m_currentVideoSettings.m_Contrast            = loadedChannelSettings.m_Contrast;
    g_settings.m_currentVideoSettings.m_Gamma               = loadedChannelSettings.m_Gamma;
    g_settings.m_currentVideoSettings.m_Crop                = loadedChannelSettings.m_Crop;
    g_settings.m_currentVideoSettings.m_CropLeft            = loadedChannelSettings.m_CropLeft;
    g_settings.m_currentVideoSettings.m_CropRight           = loadedChannelSettings.m_CropRight;
    g_settings.m_currentVideoSettings.m_CropTop             = loadedChannelSettings.m_CropTop;
    g_settings.m_currentVideoSettings.m_CropBottom          = loadedChannelSettings.m_CropBottom;
    g_settings.m_currentVideoSettings.m_CustomPixelRatio    = loadedChannelSettings.m_CustomPixelRatio;
    g_settings.m_currentVideoSettings.m_CustomZoomAmount    = loadedChannelSettings.m_CustomZoomAmount;
    g_settings.m_currentVideoSettings.m_CustomVerticalShift = loadedChannelSettings.m_CustomVerticalShift;
    g_settings.m_currentVideoSettings.m_NoiseReduction      = loadedChannelSettings.m_NoiseReduction;
    g_settings.m_currentVideoSettings.m_Sharpness           = loadedChannelSettings.m_Sharpness;
    g_settings.m_currentVideoSettings.m_InterlaceMethod     = loadedChannelSettings.m_InterlaceMethod;
    g_settings.m_currentVideoSettings.m_OutputToAllSpeakers = loadedChannelSettings.m_OutputToAllSpeakers;
    g_settings.m_currentVideoSettings.m_AudioDelay          = loadedChannelSettings.m_AudioDelay;
    g_settings.m_currentVideoSettings.m_AudioStream         = loadedChannelSettings.m_AudioStream;
    g_settings.m_currentVideoSettings.m_SubtitleOn          = loadedChannelSettings.m_SubtitleOn;
    g_settings.m_currentVideoSettings.m_SubtitleDelay       = loadedChannelSettings.m_SubtitleDelay;
    g_settings.m_currentVideoSettings.m_CustomNonLinStretch = loadedChannelSettings.m_CustomNonLinStretch;
    g_settings.m_currentVideoSettings.m_ScalingMethod       = loadedChannelSettings.m_ScalingMethod;
    g_settings.m_currentVideoSettings.m_PostProcess         = loadedChannelSettings.m_PostProcess;

    /* only change the view mode if it's different */
    if (g_settings.m_currentVideoSettings.m_ViewMode != loadedChannelSettings.m_ViewMode)
    {
      g_settings.m_currentVideoSettings.m_ViewMode = loadedChannelSettings.m_ViewMode;

      g_renderManager.SetViewMode(g_settings.m_currentVideoSettings.m_ViewMode);
      g_settings.m_currentVideoSettings.m_CustomZoomAmount = g_settings.m_fZoomAmount;
      g_settings.m_currentVideoSettings.m_CustomPixelRatio = g_settings.m_fPixelRatio;
    }

    /* only change the subtitle stream, if it's different */
    if (g_settings.m_currentVideoSettings.m_SubtitleStream != loadedChannelSettings.m_SubtitleStream)
    {
      g_settings.m_currentVideoSettings.m_SubtitleStream = loadedChannelSettings.m_SubtitleStream;

      g_application.m_pPlayer->SetSubtitle(g_settings.m_currentVideoSettings.m_SubtitleStream);
    }

    /* only change the audio stream if it's different */
    if (g_application.m_pPlayer->GetAudioStream() != g_settings.m_currentVideoSettings.m_AudioStream)
      g_application.m_pPlayer->SetAudioStream(g_settings.m_currentVideoSettings.m_AudioStream);

    g_application.m_pPlayer->SetAVDelay(g_settings.m_currentVideoSettings.m_AudioDelay);
    g_application.m_pPlayer->SetDynamicRangeCompression((long)(g_settings.m_currentVideoSettings.m_VolumeAmplification * 100));
    g_application.m_pPlayer->SetSubtitleVisible(g_settings.m_currentVideoSettings.m_SubtitleOn);
    g_application.m_pPlayer->SetSubTitleDelay(g_settings.m_currentVideoSettings.m_SubtitleDelay);
  }
}

void CPVRManager::SetPlayingGroup(CPVRChannelGroup *group)
{
  CSingleLock lock(m_critSection);

  if (group == NULL)
    return;

  bool bChanged(false);
  if (group->IsRadio())
  {
    bChanged = m_currentRadioGroup == NULL || *m_currentRadioGroup != *group;
    m_currentRadioGroup = group;
  }
  else
  {
    bChanged = m_currentTVGroup == NULL || *m_currentTVGroup != *group;
    m_currentTVGroup = group;
  }

  /* set this group as selected group and set channel numbers */
  if (bChanged)
    group->SetSelectedGroup();
}

CPVRChannelGroup *CPVRManager::GetPlayingGroup(bool bRadio /* = false */)
{
  CSingleLock lock(m_critSection);

  if (bRadio && !m_currentRadioGroup)
    SetPlayingGroup((CPVRChannelGroup *) m_channelGroups->GetGroupAllRadio());
  else if (!bRadio &&!m_currentTVGroup)
    SetPlayingGroup((CPVRChannelGroup *) m_channelGroups->GetGroupAllTV());

  return bRadio ? m_currentRadioGroup : m_currentTVGroup;
}

bool CPVRManager::IsSelectedGroup(const CPVRChannelGroup &group) const
{
  CSingleLock lock(m_critSection);

  return (group.IsRadio() && m_currentRadioGroup && *m_currentRadioGroup == group) ||
      (!group.IsRadio() && m_currentTVGroup && *m_currentTVGroup == group);
}

bool CPVRRecordingsUpdateJob::DoWork(void)
{
  g_PVRRecordings->Update();
  return true;
}

bool CPVRTimersUpdateJob::DoWork(void)
{
  return g_PVRTimers->Update();
}

bool CPVRChannelsUpdateJob::DoWork(void)
{
  return g_PVRChannelGroups->Update(true);
}

bool CPVRChannelGroupsUpdateJob::DoWork(void)
{
  return g_PVRChannelGroups->Update(false);
}

bool CPVRChannelSettingsSaveJob::DoWork(void)
{
  g_PVRManager.SaveCurrentChannelSettings();
  return true;
}

bool CPVRManager::OpenLiveStream(const CPVRChannel &tag)
{
  bool bReturn = false;
  CSingleLock lock(m_critSection);

  CLog::Log(LOGDEBUG,"PVRManager - %s - opening live stream on channel '%s'",
      __FUNCTION__, tag.ChannelName().c_str());

  if ((bReturn = m_addons->OpenLiveStream(tag)) != false)
  {
    if(m_currentFile)
    {
      delete m_currentFile;
    }
    m_currentFile = new CFileItem(tag);

    LoadCurrentChannelSettings();
  }

  return bReturn;
}

bool CPVRManager::OpenRecordedStream(const CPVRRecording &tag)
{
  bool bReturn = false;
  CSingleLock lock(m_critSection);

  CLog::Log(LOGDEBUG,"PVRManager - %s - opening recorded stream '%s'",
      __FUNCTION__, tag.m_strFile.c_str());

  if ((bReturn = m_addons->OpenRecordedStream(tag)) != false)
  {
    delete m_currentFile;
    m_currentFile = new CFileItem(tag);
  }

  return bReturn;
}

void CPVRManager::CloseStream(void)
{
  CSingleLock lock(m_critSection);

  if (m_addons->IsReadingLiveStream())
  {
    CPVRChannel channel;
    if (m_addons->GetPlayingChannel(&channel))
    {
      /* store current time in iLastWatched */
      time_t tNow;
      CDateTime::GetCurrentDateTime().GetAsTime(tNow);
      channel.SetLastWatched(tNow, true);
    }
  }

  m_addons->CloseStream();
  if (m_currentFile)
  {
    delete m_currentFile;
    m_currentFile = NULL;
  }
}

void CPVRManager::UpdateCurrentFile(void)
{
  CSingleLock lock(m_critSection);
  if (m_currentFile)
    UpdateItem(*m_currentFile);
}

bool CPVRManager::UpdateItem(CFileItem& item)
{
  /* Don't update if a recording is played */
  if (item.IsPVRRecording())
    return true;

  if (!item.IsPVRChannel())
  {
    CLog::Log(LOGERROR, "CPVRManager - %s - no channel tag provided", __FUNCTION__);
    return false;
  }

  CSingleLock lock(m_critSection);
  if (*m_currentFile->GetPVRChannelInfoTag() == *item.GetPVRChannelInfoTag())
    return false;

  g_application.CurrentFileItem() = *m_currentFile;
  g_infoManager.SetCurrentItem(*m_currentFile);

  CPVRChannel* channelTag = item.GetPVRChannelInfoTag();
  const CPVREpgInfoTag* epgTagNow = channelTag->GetEPGNow();

  if (channelTag->IsRadio())
  {
    CMusicInfoTag* musictag = item.GetMusicInfoTag();
    if (musictag)
    {
      musictag->SetTitle(epgTagNow ? epgTagNow->Title() : g_localizeStrings.Get(19055));
      musictag->SetGenre(epgTagNow ? epgTagNow->Genre() : "");
      musictag->SetDuration(epgTagNow ? epgTagNow->GetDuration() : 3600);
      musictag->SetURL(channelTag->Path());
      musictag->SetArtist(channelTag->ChannelName());
      musictag->SetAlbumArtist(channelTag->ChannelName());
      musictag->SetLoaded(true);
      musictag->SetComment("");
      musictag->SetLyrics("");
    }
  }
  else
  {
    CVideoInfoTag *videotag = item.GetVideoInfoTag();
    if (videotag)
    {
      videotag->m_strTitle = epgTagNow ? epgTagNow->Title() : g_localizeStrings.Get(19055);
      videotag->m_strGenre = epgTagNow ? epgTagNow->Genre() : "";
      videotag->m_strPath = channelTag->Path();
      videotag->m_strFileNameAndPath = channelTag->Path();
      videotag->m_strPlot = epgTagNow ? epgTagNow->Plot() : "";
      videotag->m_strPlotOutline = epgTagNow ? epgTagNow->PlotOutline() : "";
      videotag->m_iEpisode = epgTagNow ? epgTagNow->EpisodeNum() : 0;
    }
  }

  CPVRChannel* tagPrev = item.GetPVRChannelInfoTag();
  if (tagPrev && tagPrev->ChannelNumber() != m_LastChannel)
  {
    m_LastChannel         = tagPrev->ChannelNumber();
    m_LastChannelChanged  = CTimeUtils::GetTimeMS();
  }
  if (CTimeUtils::GetTimeMS() - m_LastChannelChanged >= (unsigned int) g_guiSettings.GetInt("pvrplayback.channelentrytimeout") && m_LastChannel != m_PreviousChannel[m_PreviousChannelIndex])
     m_PreviousChannel[m_PreviousChannelIndex ^= 1] = m_LastChannel;

  return false;
}

bool CPVRManager::StartPlayback(const CPVRChannel *channel, bool bPreview /* = false */)
{
  bool bReturn = false;
  g_settings.m_bStartVideoWindowed = bPreview;

  if (g_application.PlayFile(CFileItem(*channel)))
  {
    CLog::Log(LOGNOTICE, "PVRManager - %s - started playback on channel '%s'",
        __FUNCTION__, channel->ChannelName().c_str());
    bReturn = true;
  }
  else
  {
    CLog::Log(LOGERROR, "PVRManager - %s - failed to start playback on channel '%s'",
        __FUNCTION__, channel ? channel->ChannelName().c_str() : "NULL");
  }

  return bReturn;
}

bool CPVRManager::PerformChannelSwitch(const CPVRChannel &channel, bool bPreview)
{
  CSingleLock lock(m_critSection);

  CLog::Log(LOGDEBUG, "PVRManager - %s - switching to channel '%s'",
      __FUNCTION__, channel.ChannelName().c_str());

  /* make sure that channel settings are persisted */
  if (!bPreview)
    SaveCurrentChannelSettings();

  if (m_currentFile)
  {
    delete m_currentFile;
    m_currentFile = NULL;
  }

  if (!bPreview && (channel.ClientID() < 0 || !m_addons->SwitchChannel(channel)))
  {
    CLog::Log(LOGERROR, "PVRManager - %s - failed to switch to channel '%s'",
        __FUNCTION__, channel.ChannelName().c_str());
    CGUIDialogOK::ShowAndGetInput(19033,0,19136,0);
    return false;
  }

  m_currentFile = new CFileItem(channel);

  if (!bPreview)
  {
    LoadCurrentChannelSettings();

    CLog::Log(LOGNOTICE, "PVRManager - %s - switched to channel '%s'",
        __FUNCTION__, channel.ChannelName().c_str());
  }

  return true;
}

int CPVRManager::GetTotalTime(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return 0;
  lock.Leave();

  return !m_guiInfo ? 0 : m_guiInfo->GetDuration();
}

int CPVRManager::GetStartTime(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return 0;
  lock.Leave();

  return !m_guiInfo ? 0 : m_guiInfo->GetStartTime();
}

bool CPVRManager::TranslateBoolInfo(DWORD dwInfo) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return !m_guiInfo ? false : m_guiInfo->TranslateBoolInfo(dwInfo);
}

bool CPVRManager::TranslateCharInfo(DWORD dwInfo, CStdString &strValue) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return !m_guiInfo ? false : m_guiInfo->TranslateCharInfo(dwInfo, strValue);
}

int CPVRManager::TranslateIntInfo(DWORD dwInfo) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return 0;
  lock.Leave();

  return !m_guiInfo ? 0 : m_guiInfo->TranslateIntInfo(dwInfo);
}

bool CPVRManager::HasTimer(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return !m_guiInfo ? false : m_guiInfo->HasTimers();
}

bool CPVRManager::IsRecording(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return !m_guiInfo ? false : m_guiInfo->IsRecording();
}

void CPVRManager::ShowPlayerInfo(int iTimeout)
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return;
  lock.Leave();

  m_guiInfo->ShowPlayerInfo(iTimeout);
}

void CPVRManager::LocalizationChanged(void)
{
  CSingleLock lock(m_critSection);
  if (m_bLoaded)
  {
    m_channelGroups->GetGroupAllRadio()->CheckGroupName();
    m_channelGroups->GetGroupAllTV()->CheckGroupName();
  }
}

bool CPVRManager::IsRunning(void) const
{
  CSingleLock lock(m_critSection);
  return !m_bStop;
}

bool CPVRManager::IsPlayingTV(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return m_addons->IsPlayingTV();
}

bool CPVRManager::IsPlayingRadio(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return m_addons->IsPlayingRadio();
}

bool CPVRManager::IsPlayingRecording(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return m_addons->IsPlayingRecording();
}

bool CPVRManager::IsRunningChannelScan(void) const
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return false;
  lock.Leave();

  return m_addons->IsRunningChannelScan();
}

PVR_ADDON_CAPABILITIES *CPVRManager::GetCurrentClientProperties(void)
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return NULL;
  lock.Leave();

  return m_addons->GetCurrentAddonCapabilities();
}

void CPVRManager::StartChannelScan(void)
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return;
  lock.Leave();

  return m_addons->StartChannelScan();
}

void CPVRManager::SearchMissingChannelIcons(void)
{
  CSingleLock lock(m_critSection);
  if (!m_bLoaded)
    return;
  lock.Leave();

  return m_channelGroups->SearchMissingChannelIcons();
}

bool CPVRManager::IsJobPending(const char *strJobName) const
{
  bool bReturn(false);
  CSingleLock lock(m_critSectionTriggers);
  if (!m_bLoaded)
    return bReturn;

  for (unsigned int iJobPtr = 0; iJobPtr < m_pendingUpdates.size(); iJobPtr++)
  {
    if (!strcmp(m_pendingUpdates.at(iJobPtr)->GetType(), "pvr-update-recordings"))
    {
      bReturn = true;
      break;
    }
  }

  return bReturn;
}

void CPVRManager::TriggerRecordingsUpdate(void)
{
  CSingleLock lock(m_critSectionTriggers);
  if (!m_bLoaded)
    return;

  if (IsJobPending("pvr-update-recordings"))
    return;

  m_pendingUpdates.push_back(new CPVRRecordingsUpdateJob());

  lock.Leave();
  SetEvent(m_triggerEvent);
}

void CPVRManager::TriggerTimersUpdate(void)
{
  CSingleLock lock(m_critSectionTriggers);
  if (!m_bLoaded)
    return;

  if (IsJobPending("pvr-update-timers"))
    return;

  m_pendingUpdates.push_back(new CPVRTimersUpdateJob());

  lock.Leave();
  SetEvent(m_triggerEvent);
}

void CPVRManager::TriggerChannelsUpdate(void)
{
  CSingleLock lock(m_critSectionTriggers);
  if (!m_bLoaded)
    return;

  if (IsJobPending("pvr-update-channels"))
    return;

  m_pendingUpdates.push_back(new CPVRChannelsUpdateJob());

  lock.Leave();
  SetEvent(m_triggerEvent);
}

void CPVRManager::TriggerChannelGroupsUpdate(void)
{
  CSingleLock lock(m_critSectionTriggers);
  if (!m_bLoaded)
    return;

  if (IsJobPending("pvr-update-channelgroups"))
    return;

  m_pendingUpdates.push_back(new CPVRChannelGroupsUpdateJob());

  lock.Leave();
  SetEvent(m_triggerEvent);
}

void CPVRManager::TriggerSaveChannelSettings(void)
{
  CSingleLock lock(m_critSectionTriggers);
  if (!m_bLoaded)
    return;

  if (IsJobPending("pvr-save-channelsettings"))
    return;

  m_pendingUpdates.push_back(new CPVRChannelSettingsSaveJob());

  lock.Leave();
  SetEvent(m_triggerEvent);
}

void CPVRManager::ExecutePendingJobs(void)
{
  CSingleLock lock(m_critSectionTriggers);

  while (m_pendingUpdates.size() > 0)
  {
    CJob *job = m_pendingUpdates.at(0);
    m_pendingUpdates.erase(m_pendingUpdates.begin());
    lock.Leave();

    job->DoWork();
    delete job;

    lock.Enter();
  }

  ResetEvent(m_triggerEvent);
}
