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
/*
 * vendor hidl interface for wpa_supplicant daemon
 *
 */

#include "hidl_manager.h"
#include "hidl_return_util.h"
#include "iface_config_utils.h"
#include "misc_utils.h"
#include "sta_iface.h"
#include "vendorsta_iface.h"

extern "C" {
#include "utils/eloop.h"
#include "gas_query.h"
#include "interworking.h"
#include "hs20_supplicant.h"
#include "wps_supplicant.h"
}

namespace vendor {
namespace qti {
namespace hardware {
namespace wifi {
namespace supplicantvendor {
namespace V2_0 {
namespace Implementation {
using android::hardware::wifi::supplicant::V1_1::implementation::hidl_return_util::validateAndCall;

VendorStaIface::VendorStaIface(struct wpa_global *wpa_global, const char ifname[])
    : wpa_global_(wpa_global), ifname_(ifname), is_valid_(true)
{
}

void VendorStaIface::invalidate() { is_valid_ = false; }
bool VendorStaIface::isValid()
{
	return (is_valid_ && (retrieveIfacePtr() != nullptr));
}

Return<void> VendorStaIface::registerVendorCallback(
    const android::sp<ISupplicantVendorStaIfaceCallback> &callback,
    registerVendorCallback_cb _hidl_cb)
{
	return validateAndCall(
	    this, SupplicantStatusCode::FAILURE_IFACE_INVALID,
	    &VendorStaIface::registerCallbackInternal, _hidl_cb, callback);
}

Return<void> VendorStaIface::filsHlpFlushRequest(filsHlpFlushRequest_cb _hidl_cb)
{
        return validateAndCall(
            this, SupplicantStatusCode::FAILURE_IFACE_INVALID,
            &VendorStaIface::filsHlpFlushRequestInternal, _hidl_cb);
}

Return<void> VendorStaIface::filsHlpAddRequest(
    const hidl_array<uint8_t, 6> &dst_mac, const hidl_vec<uint8_t> &pkt,
    filsHlpAddRequest_cb _hidl_cb)
{
        return validateAndCall(
            this, SupplicantStatusCode::FAILURE_IFACE_INVALID,
            &VendorStaIface::filsHlpAddRequestInternal, _hidl_cb, dst_mac, pkt);
}

Return<void> VendorStaIface::getCapabilities(
    const hidl_string &capa_type, getCapabilities_cb _hidl_cb)
{
        return validateAndCall(
            this, SupplicantStatusCode::FAILURE_IFACE_INVALID,
            &VendorStaIface::getCapabilitiesInternal, _hidl_cb, capa_type);
}

Return<void> VendorStaIface::getVendorNetwork(
    SupplicantNetworkId id, getVendorNetwork_cb _hidl_cb)
{
	return validateAndCall(
	    this, SupplicantStatusCode::FAILURE_IFACE_INVALID,
	    &VendorStaIface::getNetworkInternal, _hidl_cb, id);
}

SupplicantStatus VendorStaIface::registerCallbackInternal(
    const android::sp<ISupplicantVendorStaIfaceCallback> &callback)
{
	HidlManager *hidl_manager = HidlManager::getInstance();
	if (!hidl_manager ||
	    hidl_manager->addVendorStaIfaceCallbackHidlObject(ifname_, callback)) {
		wpa_printf(MSG_INFO, "return failure vendor staiface callback");
		return {SupplicantStatusCode::FAILURE_UNKNOWN, ""};
	}
	return {SupplicantStatusCode::SUCCESS, ""};
}

SupplicantStatus VendorStaIface::filsHlpFlushRequestInternal()
{
#ifdef CONFIG_FILS
        struct wpa_supplicant *wpa_s = retrieveIfacePtr();

        wpas_flush_fils_hlp_req(wpa_s);
        return {SupplicantStatusCode::SUCCESS, ""};
#else /* CONFIG_FILS */
        return {SupplicantStatusCode::FAILURE_UNKNOWN, ""};
#endif /* CONFIG_FILS */
}

SupplicantStatus VendorStaIface::filsHlpAddRequestInternal(
    const std::array<uint8_t, 6> &dst_mac, const std::vector<uint8_t> &pkt)
{
#ifdef CONFIG_FILS
        struct wpa_supplicant *wpa_s = retrieveIfacePtr();
        struct fils_hlp_req *req;

        req = (struct fils_hlp_req *)os_zalloc(sizeof(*req));
        if (!req)
                return {SupplicantStatusCode::FAILURE_UNKNOWN, ""};

        os_memcpy(req->dst, dst_mac.data(), ETH_ALEN);

        req->pkt = wpabuf_alloc_copy(pkt.data(), pkt.size());
        if (!req->pkt) {
                wpabuf_free(req->pkt);
                os_free(req);
                return {SupplicantStatusCode::FAILURE_UNKNOWN, ""};
        }

        dl_list_add_tail(&wpa_s->fils_hlp_req, &req->list);
        return {SupplicantStatusCode::SUCCESS, ""};
#else /* CONFIG_FILS */
        return {SupplicantStatusCode::FAILURE_UNKNOWN, ""};
#endif /* CONFIG_FILS */
}

std::pair<SupplicantStatus, android::sp<ISupplicantVendorNetwork>>
VendorStaIface::getNetworkInternal(SupplicantNetworkId id)
{
	android::sp<ISupplicantVendorStaNetwork> network;
	struct wpa_supplicant *wpa_s = retrieveIfacePtr();
	struct wpa_ssid *ssid = wpa_config_get_network(wpa_s->conf, id);
	if (!ssid) {
		return {{SupplicantStatusCode::FAILURE_NETWORK_UNKNOWN, ""},
			network};
	}
	HidlManager *hidl_manager = HidlManager::getInstance();
	if (!hidl_manager ||
	    hidl_manager->getVendorStaNetworkHidlObjectByIfnameAndNetworkId(
		wpa_s->ifname, ssid->id, &network)) {
		return {{SupplicantStatusCode::FAILURE_UNKNOWN, ""}, network};
	}
	return {{SupplicantStatusCode::SUCCESS, ""}, network};
}

std::pair<SupplicantStatus, std::string>
VendorStaIface::getCapabilitiesInternal(const std::string &capa_type)
{
        uint8_t get_capability = 0;
        struct wpa_supplicant* wpa_s = retrieveIfacePtr();
        if (!wpa_s) {
                return {{SupplicantStatusCode::FAILURE_IFACE_UNKNOWN, ""},
                        ""};
        }

        get_capability = wpa_supplicant_ctrl_iface_get_capability(
                                        wpa_s, capa_type.c_str(),
                                        device_capabilities, DEVICE_CAPA_SIZE);
        if(get_capability > 0) {
                wpa_printf(MSG_INFO, "getCapabilitiesInternal capabilities: %s",
                                device_capabilities);
                return {{SupplicantStatusCode::SUCCESS, ""}, device_capabilities};
        }
        else
                return {{SupplicantStatusCode::FAILURE_UNKNOWN, ""}, ""};

}

/**
 * Retrieve the underlying |wpa_supplicant| struct
 * pointer for this iface.
 * If the underlying iface is removed, then all RPC method calls on this object
 * will return failure.
 */
wpa_supplicant *VendorStaIface::retrieveIfacePtr()
{
	return wpa_supplicant_get_iface(wpa_global_, ifname_.c_str());
}
}  // namespace implementation
}  // namespace V2_0
}  // namespace supplicant
}  // namespace wifi
}  // namespace hardware
}  // namespace qti
}  // namespace vendor
