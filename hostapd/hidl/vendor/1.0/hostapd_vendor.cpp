/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/* vendor hidl interface for hostapd daemon */

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <iostream>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <hidl/HidlSupport.h>

#include "hostapd_vendor.h"
#include "qsap_handler.h"
#include "hidl_return_util.h"

extern "C"
{
#include "utils/eloop.h"
}

// This HIDL implementation for hostapd add or update a hostapd.conf dynamically
// for each interface. If file doesn't exist, it would copy template file and
// then update params otherwise simply update params in existing file.
// conf file naming convention would be as per QSAP implementation:
// hostapd.conf
// hostapd_2g.conf
// hostapd_5g.conf
namespace {
constexpr char kConfFileNameFmt[] = "/data/vendor/wifi/hostapd/hostapd%s.conf";
constexpr char kQsapSetFmt[] = "softap qccmd set%s %s=%s";

using android::hardware::hidl_array;
using android::base::StringPrintf;
using android::hardware::wifi::hostapd::V1_0::IHostapd;
using vendor::qti::hardware::wifi::hostapd::V1_0::IHostapdVendor;

using namespace vendor::qti::hardware::wifi::hostapd::V1_0::implementation::qsap_handler;

// wrapper to call qsap command.
int qsap_cmd(std::string cmd) {
	const int max_arg_size = 10;
	char *data[max_arg_size];
	char **argv = data;
	int argc = 0;

	wpa_printf(MSG_DEBUG, "qsap command: %s", cmd.c_str());

	/* Tokenize command to char array */
	std::istringstream buf(cmd);
	std::istream_iterator<std::string> beg(buf), end;
	std::vector<std::string> tokens(beg, end);

	for(auto& s: tokens) {
		if (argc >= max_arg_size) {
			wpa_printf(MSG_ERROR, "Command too long");
			return -1;
		}
		data[argc] = strdup(s.c_str());
		argc++;
	}

	if (run_qsap_cmd(argc, argv)) {
		wpa_printf(MSG_ERROR, "Unhandled or wrong QSAP command");
		return -1;
	}

	return 0;
}

std::string AddOrUpdateHostapdConfig(
    const IHostapdVendor::VendorIfaceParams& v_iface_params,
    const IHostapd::NetworkParams& nw_params)
{
	IHostapd::IfaceParams iface_params = v_iface_params.ifaceParams;
	if (nw_params.ssid.size() >
	    static_cast<uint32_t>(
		IHostapd::ParamSizeLimits::SSID_MAX_LEN_IN_BYTES)) {
		wpa_printf(
		    MSG_ERROR, "Invalid SSID size: %zu", nw_params.ssid.size());
		return "";
	}
	if ((nw_params.encryptionType != IHostapd::EncryptionType::NONE) &&
	    (nw_params.pskPassphrase.size() <
		 static_cast<uint32_t>(
		     IHostapd::ParamSizeLimits::
			 WPA2_PSK_PASSPHRASE_MIN_LEN_IN_BYTES) ||
	     nw_params.pskPassphrase.size() >
		 static_cast<uint32_t>(
		     IHostapd::ParamSizeLimits::
			 WPA2_PSK_PASSPHRASE_MAX_LEN_IN_BYTES))) {
		wpa_printf(
		    MSG_ERROR, "Invalid psk passphrase size: %zu",
		    nw_params.pskPassphrase.size());
		return "";
	}

	// Decide configuration file to be used.
	bool dual_mode = false;
	std::string dual_str;
	std::string file_path;

	if (v_iface_params.bridgeIfaceName.empty()) {
		file_path = StringPrintf(kConfFileNameFmt, "");
		dual_str = "";
	} else if (iface_params.channelParams.band == IHostapd::Band::BAND_2_4_GHZ) {
		file_path = StringPrintf(kConfFileNameFmt, "_dual2g");
		dual_mode = true;
		dual_str = " dual2g";
	} else if (iface_params.channelParams.band == IHostapd::Band::BAND_5_GHZ) {
		file_path = StringPrintf(kConfFileNameFmt, "_dual5g");
		dual_mode = true;
		dual_str = " dual5g";
	}
	const char *dual_mode_str = dual_str.c_str();

	// SSID string
	std::stringstream ss;
	ss << std::hex;
	ss << std::setfill('0');
	for (uint8_t b : nw_params.ssid) {
		ss << std::setw(2) << static_cast<unsigned int>(b);
	}
	const std::string ssid_as_string = ss.str();
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ssid2", ssid_as_string.c_str()));

	switch (nw_params.encryptionType) {
	case IHostapd::EncryptionType::NONE:
		// no security params
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "security_mode", "0"));
		break;
	case IHostapd::EncryptionType::WPA:
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "security_mode", "4"));
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "wpa_passphrase", nw_params.pskPassphrase.c_str()));
		break;
	case IHostapd::EncryptionType::WPA2:
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "security_mode", "3"));
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "wpa_passphrase", nw_params.pskPassphrase.c_str()));
		break;
	default:
		wpa_printf(MSG_ERROR, "Unknown encryption type");
		return "";
	}

	if (iface_params.channelParams.enableAcs) {
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "channel", "0"));
		// TODO: add support for acs_exclude_dfs param in qsap then add config here.
	} else {
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "channel", std::to_string(iface_params.channelParams.channel).c_str()));
	}

	switch (iface_params.channelParams.band) {
	case IHostapd::Band::BAND_2_4_GHZ:
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "hw_mode", "g"));
		break;
	case IHostapd::Band::BAND_5_GHZ:
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "hw_mode", "a"));
		if (iface_params.channelParams.enableAcs) {
			qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ht_capab", "[HT40+]"));
			qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "vht_oper_chwidth", "1"));
		}
		break;
	case IHostapd::Band::BAND_ANY:
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "hw_mode", "any"));
		if (iface_params.channelParams.enableAcs) {
			qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ht_capab", "[HT40+]"));
			qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "vht_oper_chwidth", "1"));
		}
		break;
	default:
		wpa_printf(MSG_ERROR, "Invalid band");
		return "";
	}

	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "interface", iface_params.ifaceName.c_str()));
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "driver", "nl80211"));
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ctrl_interface", "/data/vendor/wifi/hostapd/ctrl"));
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ieee80211n", iface_params.hwModeParams.enable80211N ? "1" : "0"));
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ieee80211ac", iface_params.hwModeParams.enable80211AC ? "1" : "0"));
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "ignore_broadcast_ssid", nw_params.isHidden ? "1" : "0"));
	qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "wowlan_triggers", "any"));

	if (dual_mode)
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "bridge", v_iface_params.bridgeIfaceName.c_str()));

	if (!v_iface_params.countryCode.empty())
		qsap_cmd(StringPrintf(kQsapSetFmt, dual_mode_str, "country_code", v_iface_params.countryCode.c_str()));

	return file_path;
}

template <class CallbackType>
int addIfaceCallbackHidlObjectToMap(
    const std::string &ifname, const android::sp<CallbackType> &callback,
    std::map<const std::string, std::vector<android::sp<CallbackType>>>
	&callbacks_map)
{
	if (ifname.empty())
		return 1;

	auto iface_callback_map_iter = callbacks_map.find(ifname);
	if (iface_callback_map_iter == callbacks_map.end())
		return 1;

	auto &iface_callback_list = iface_callback_map_iter->second;

        std::vector<android::sp<CallbackType>> &callback_list = iface_callback_list;
	callback_list.push_back(callback);
	return 0;
}

template <class CallbackType>
void callWithEachIfaceCallback(
    const std::string &ifname,
    const std::function<
	android::hardware::Return<void>(android::sp<CallbackType>)> &method,
    const std::map<const std::string, std::vector<android::sp<CallbackType>>>
	&callbacks_map)
{
	if (ifname.empty())
		return;

	auto iface_callback_map_iter = callbacks_map.find(ifname);
	if (iface_callback_map_iter == callbacks_map.end())
		return;

	const auto &iface_callback_list = iface_callback_map_iter->second;
	for (const auto &callback : iface_callback_list) {
		if (!method(callback).isOk()) {
			wpa_printf(
			    MSG_ERROR, "Failed to invoke Hostapd HIDL iface callback");
		}
	}
}
}  // namespace



namespace vendor {
namespace qti {
namespace hardware {
namespace wifi {
namespace hostapd {
namespace V1_0 {
namespace implementation {

using namespace android::hardware;
using namespace android::hardware::wifi::hostapd::V1_0;
using namespace android::hardware::wifi::hostapd::V1_0::implementation::hidl_return_util;

using android::base::StringPrintf;
using qsap_handler::run_qsap_cmd;

HostapdVendor::HostapdVendor(struct hapd_interfaces* interfaces)
    : interfaces_(interfaces)
{}

Return<void> HostapdVendor::addVendorAccessPoint(
    const VendorIfaceParams& iface_params, const NetworkParams& nw_params,
    addVendorAccessPoint_cb _hidl_cb)
{
	return call(
	    this, &HostapdVendor::addVendorAccessPointInternal, _hidl_cb, iface_params,
	    nw_params);
}

Return<void> HostapdVendor::removeVendorAccessPoint(
    const hidl_string& iface_name, removeVendorAccessPoint_cb _hidl_cb)
{
	return call(
	    this, &HostapdVendor::removeVendorAccessPointInternal, _hidl_cb, iface_name);
}

Return<void> HostapdVendor::setHostapdParams(
    const hidl_string& cmd, setHostapdParams_cb _hidl_cb)
{
	return call(
	    this, &HostapdVendor::setHostapdParamsInternal, _hidl_cb, cmd);
}

Return<void> HostapdVendor::registerVendorCallback(
    const hidl_string& iface_name,
    const android::sp<IHostapdVendorIfaceCallback> &callback,
    registerVendorCallback_cb _hidl_cb)
{
	return call(
	    this, &HostapdVendor::registerCallbackInternal, _hidl_cb, iface_name, callback);
}

HostapdStatus HostapdVendor::addVendorAccessPointInternal(
    const VendorIfaceParams& v_iface_params, const NetworkParams& nw_params)
{
	IfaceParams iface_params = v_iface_params.ifaceParams;
	if (hostapd_get_iface(interfaces_, iface_params.ifaceName.c_str())) {
		wpa_printf(
		    MSG_ERROR, "Interface %s already present",
		    iface_params.ifaceName.c_str());
		return {HostapdStatusCode::FAILURE_IFACE_EXISTS, ""};
	}
	const auto conf_file_path =
	    AddOrUpdateHostapdConfig(v_iface_params, nw_params);
	if (conf_file_path.empty()) {
		wpa_printf(MSG_ERROR, "Failed to add or update config file");
		return {HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}

	std::string add_iface_param_str = StringPrintf(
	    "%s config=%s", iface_params.ifaceName.c_str(),
	    conf_file_path.c_str());
	std::vector<char> add_iface_param_vec(
	    add_iface_param_str.begin(), add_iface_param_str.end() + 1);
	if (hostapd_add_iface(interfaces_, add_iface_param_vec.data()) < 0) {
		wpa_printf(
		    MSG_ERROR, "Adding interface %s failed",
		    add_iface_param_str.c_str());
		return {HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	struct hostapd_data* iface_hapd =
	    hostapd_get_iface(interfaces_, iface_params.ifaceName.c_str());
	WPA_ASSERT(iface_hapd != nullptr && iface_hapd->iface != nullptr);
	if (hostapd_enable_iface(iface_hapd->iface) < 0) {
		wpa_printf(
		    MSG_ERROR, "Enabling interface %s failed",
		    iface_params.ifaceName.c_str());
		return {HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	return {HostapdStatusCode::SUCCESS, ""};
}

HostapdStatus HostapdVendor::removeVendorAccessPointInternal(const std::string& iface_name)
{
	std::vector<char> remove_iface_param_vec(
	    iface_name.begin(), iface_name.end() + 1);
	if (hostapd_remove_iface(interfaces_, remove_iface_param_vec.data()) <
	    0) {
		wpa_printf(
		    MSG_ERROR, "Removing interface %s failed",
		    iface_name.c_str());
		return {HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	return {HostapdStatusCode::SUCCESS, ""};
}

HostapdStatus HostapdVendor::setHostapdParamsInternal(const std::string& cmd)
{
	if (qsap_cmd(cmd))
		return {HostapdStatusCode::FAILURE_UNKNOWN, ""};

	return {HostapdStatusCode::SUCCESS, ""};
}

int HostapdVendor::onStaConnected(uint8_t * Macaddr, char *iface_name)
{
	// Invoke the |onStaConnected| method on all Hostapd registered callbacks.
	callWithEachHostapdIfaceCallback(
	    iface_name,
	    std::bind(
	    &IHostapdVendorIfaceCallback::onStaConnected, std::placeholders::_1,
	    Macaddr));
	return 0;
}

int HostapdVendor::onStaDisconnected(uint8_t * Macaddr, char *iface_name)
{
	// Invoke the |onStaDisconnected| method on all Hostapd registered callbacks.
	callWithEachHostapdIfaceCallback(
	    iface_name,
	    std::bind(
	    &IHostapdVendorIfaceCallback::onStaDisconnected, std::placeholders::_1,
	    Macaddr));
	return 0;
}

HostapdStatus HostapdVendor::registerCallbackInternal(
    const std::string& iface_name,
    const android::sp<IHostapdVendorIfaceCallback> &callback)
{
	if(addVendorIfaceCallbackHidlObject(iface_name, callback)) {
	   wpa_printf(MSG_ERROR, "FAILED to register Hostapd Vendor IfaceCallback");
	   return {HostapdStatusCode::FAILURE_UNKNOWN, ""};
	}
	return {HostapdStatusCode::SUCCESS, ""};
}

/**
 * Add a new Hostapd vendoriface callback hidl object
 * reference to our interface callback list.
 *
 * @param ifname Name of the corresponding interface.
 * @param callback Hidl reference of the vendor ifacecallback object.
 *
 * @return 0 on success, 1 on failure.
 */
int HostapdVendor::addVendorIfaceCallbackHidlObject(
    const std::string &ifname,
    const android::sp<IHostapdVendorIfaceCallback> &callback)
{
	vendor_hostapd_callbacks_map_[ifname] =
		    std::vector<android::sp<IHostapdVendorIfaceCallback>>();
	return addIfaceCallbackHidlObjectToMap(ifname, callback, vendor_hostapd_callbacks_map_);
}

/**
 * Helper function to invoke the provided callback method on all the
 * registered |IHostapdVendorIfaceCallback| callback hidl objects.
 *
 * @param ifname Name of the corresponding interface.
 * @param method Pointer to the required hidl method from
 * |IHostapdVendorIfaceCallback|.
 */
void HostapdVendor::callWithEachHostapdIfaceCallback(
    const std::string &ifname,
    const std::function<Return<void>(android::sp<IHostapdVendorIfaceCallback>)> &method)
{
	callWithEachIfaceCallback(ifname, method, vendor_hostapd_callbacks_map_);
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace hostapd
}  // namespace wifi
}  // namespace hardware
}  // namespace qti
}  // namespace vendor
