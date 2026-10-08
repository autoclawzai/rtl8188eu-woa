# Phase 5d research notes (connect / WPA2)

## WDI structs (WDK 10.0.26100, km/wlan/2.0/TlvGenerated_.hpp)
- WDI_TASK_CONNECT_PARAMETERS { WDI_CONNECT_PARAMETERS_CONTAINER ConnectParameters }
  - container: AuthenticationAlgorithms, MulticastCipherAlgorithms, UnicastCipherAlgorithms,
    optional AssociationRequestVendorIE, ActivePhyTypeList, Disallowed/AllowedBSSIDs, RsnaAkm*.
  - BSS to join comes from WDI_CONNECT_BSS_ENTRY_CONTAINER (BSSID, beacon/probe-resp body, channel).
- WDI_ASSOCIATION_RESULT_PARAMETERS { AssociationStatus, StatusCode, ReAssociation, AuthAlgorithm,
  UnicastCipherAlgorithm, MulticastDataCipherAlgorithm, MulticastMgmtCipherAlgorithm, FourAddressSupported,
  PortAuthorized, WMMQoSEnabled, DSInfo, AssociationComebackTime, BandID, IHVAssociationStatus, ... }
- WDI_DISCONNECT_PARAMETERS { MacAddress, Disassociation80211Reason }
- WDI_SET_ADD_CIPHER_KEYS_CONTAINER { PeerMacAddress, CipherKeyID, CipherKeyTypeInfo
  {CipherAlgorithm,Direction,Static,KeyType}, ReceiveSequenceCount, CCMPKey (private blob), TKIPInfo, WEPKey, ... }
- WDI_SET_DELETE_CIPHER_KEYS_CONTAINER { PeerMacAddress, CipherKeyID, CipherKeyTypeInfo }
- WDI_SET_DEFAULT_KEY_ID_STRUCT { KeyID }
- Link state: WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS{LinkStateChangeParameters}

## Hardware (8188eu uses rtl8xxxu gen2 ops)
- report_connect = gen2: H2C cmd 0x01 (MEDIA_STATUS_RPT), parm = connect | role<<4, macid.
- H2C mailbox: poll REG_HMTFR (0x1CC) bit(n) clear, write REG_HMBOX_0 + 4*n (0x1D0), n round-robin 0..3.
- REG_MSR 0x102 link type STA=2, REG_BSSID 0x618, REG_RESPONSE_RATE_SET, REG_INIRTS_RATE_SEL = highest basic rate idx.
- CAM key write: REG_CAM_WRITE 0x674 / REG_CAM_CMD 0x670, 6 dwords per entry.

## Plan
5d-A: open-network connect + real data path (TX BE -> 2nd bulk OUT, RX 802.11->Ethernet, C2H skip).
5d-B: WPA2: EAPOL passthrough, CAM key install from SET_ADD_CIPHER_KEYS, SEC_AES TX, CCMP hdr/MIC strip.
