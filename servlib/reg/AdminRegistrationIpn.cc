/*
 *    Copyright 2015 United States Government as represented by NASA
 *       Marshall Space Flight Center. All Rights Reserved.
 *
 *    Released under the NASA Open Source Software Agreement version 1.3;
 *    You may obtain a copy of the Agreement at:
 * 
 *        http://ti.arc.nasa.gov/opensource/nosa/
 * 
 *    The subject software is provided "AS IS" WITHOUT ANY WARRANTY of any kind,
 *    either expressed, implied or statutory and this agreement does not,
 *    in any manner, constitute an endorsement by government agency of any
 *    results, designs or products resulting from use of the subject software.
 *    See the Agreement for the specific language governing permissions and
 *    limitations.
 */

#ifdef HAVE_CONFIG_H
#  include <dtn-config.h>
#endif

#include <third_party/oasys/util/ScratchBuffer.h>

#include "AdminRegistrationIpn.h"
#include "RegistrationTable.h"
#include "bundling/BundleDaemon.h"
#include "bundling/BundleProtocol.h"
#include "bundling/BundleProtocolVersion7.h"
#include "bundling/BP6_BundleStatusReport.h"
#include "bundling/BundleStatusReport.h"
#include "bundling/CborUtilBP7.h"
#include "bundling/CustodySignal.h"
#include "routing/BundleRouter.h"

#define SET_FLDNAMES(fld) \
    fld_name_ = fld; \
    cborutil.set_fld_name(fld);

#define CHECK_CBOR_ENCODE_ERR_RETURN \
    if (err && (err != CborErrorOutOfMemory)) \
    { \
      log_err("CBOR encoding error (bundle ID: %" PRIbid "): %s - %s", \
                bundle->bundleid(), fld_name(), cbor_error_string(err)); \
      return CBORUTIL_FAIL; \
    }

#define CHECK_CBOR_DECODE_ERR \
    if (err != CborNoError) \
    { \
      log_err("CBOR parsing error: %s -  %s", fld_name(), cbor_error_string(err)); \
      goto done; \
    }

#define CHECK_CBOR_STATUS \
    if (status != CBORUTIL_SUCCESS) \
    { \
      goto done; \
    }

namespace dtn {

AdminRegistrationIpn::AdminRegistrationIpn()
    : Registration(ADMIN_REGID_IPN,
                   BundleDaemon::instance()->local_eid_ipn(),
                   Registration::DEFER,
                   Registration::NEW, 0, 0)
{
    logpathf("/dtn/reg/adminipn");
    set_active(true);
}

void
AdminRegistrationIpn::deliver_bundle(Bundle* bundle)
{
    if (bundle->is_bpv6()) {
        deliver_bundle_bp6(bundle);
    } else if (bundle->is_bpv7()) {
        deliver_bundle_bp7(bundle);
    }
}

void
AdminRegistrationIpn::deliver_bundle_bp6(Bundle* bundle)
{
    u_char typecode;

    size_t payload_len = bundle->payload().length();
    oasys::ScratchBuffer<u_char*, 256> scratch(payload_len);
    const u_char* payload_buf = 
        bundle->payload().read_data(0, payload_len, scratch.buf(payload_len));
    
    log_debug("got %zu byte bundle", payload_len);
        
    bool is_delivered = true;

    if (payload_len == 0) {
        log_err("admin registration got 0 byte *%p", bundle);
        is_delivered = false;
        goto done;
    }

    if (!bundle->is_admin()) {
        if (nullptr != strstr((const char*)payload_buf, "ping"))  {
            Bundle* reply = new Bundle();
            reply->set_source(endpoint_);
            reply->set_bp_version(bundle->bp_version());
            reply->mutable_dest()->assign(bundle->source());
            reply->mutable_replyto()->assign(EndpointID::NULL_EID());
            reply->mutable_custodian()->assign(EndpointID::NULL_EID());
            reply->set_expiration_millis(bundle->expiration_millis());

            size_t ipn_echo_max_length = BundleDaemon::params_.ipn_echo_max_return_length_;

            if ((ipn_echo_max_length > 0) && (payload_len > ipn_echo_max_length)) {
                reply->mutable_payload()->set_length(ipn_echo_max_length);
                reply->mutable_payload()->write_data(bundle->payload(), 0, ipn_echo_max_length, 0);
            } else {
                reply->mutable_payload()->set_length(payload_len);
                reply->mutable_payload()->write_data(bundle->payload(), 0, payload_len, 0);
            }

            BundleDaemon::post(new BundleReceivedEvent(reply, EVENTSRC_ADMIN));
        } else {
            log_warn("non-admin *%p sent to local eid", bundle);
            is_delivered = false;
        } 
        goto done;
    }


    /*
     * As outlined in the bundle specification, the first four bits of
     * all administrative bundles hold the type code, with the
     * following values:
     *
     * 0x1     - bundle status report
     * 0x2     - custodial signal
     * 0x3     - echo request
     * 0x4     - aggregate custodial signal
     * 0x5     - announce
     * (other) - reserved
     */
    typecode = payload_buf[0] >> 4;
    
    switch(typecode) {
    case BundleProtocolVersion6::ADMIN_STATUS_REPORT:
    {
        BP6_BundleStatusReport::data_t sr_data;
        if (BP6_BundleStatusReport::parse_status_report(&sr_data, bundle))
        {
           GbofId source_gbofid(sr_data.orig_source_eid_,
                                sr_data.orig_creation_tv_,
                                (sr_data.orig_frag_length_ > 0),
                                sr_data.orig_frag_length_,
                                sr_data.orig_frag_offset_);

            char tmptxt[32];
            std::string rpt_text;
            if (sr_data.status_flags_ & BP6_BundleStatusReport::STATUS_RECEIVED)
            {
                rpt_text.append("RECEIVED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%" PRIu64, sr_data.receipt_tv_.seconds_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.status_flags_ & BP6_BundleStatusReport::STATUS_CUSTODY_ACCEPTED)
            {
                if (rpt_text.length() > 0) rpt_text.append(" & ");
                rpt_text.append("CUSTODY_ACCEPTED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%" PRIu64, sr_data.custody_tv_.seconds_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.status_flags_ & BP6_BundleStatusReport::STATUS_FORWARDED)
            {
                if (rpt_text.length() > 0) rpt_text.append(" & ");
                rpt_text.append("FORWARDED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%" PRIu64, sr_data.forwarding_tv_.seconds_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.status_flags_ & BP6_BundleStatusReport::STATUS_DELIVERED)
            {
                if (rpt_text.length() > 0) rpt_text.append(" & ");
                rpt_text.append("DELIVERED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%" PRIu64, sr_data.delivery_tv_.seconds_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.status_flags_ & BP6_BundleStatusReport::STATUS_ACKED_BY_APP)
            {
                if (rpt_text.length() > 0) rpt_text.append(" & ");
                rpt_text.append("ACKED_BY_APP at ");
                snprintf(tmptxt, sizeof(tmptxt), "%" PRIu64, sr_data.receipt_tv_.seconds_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.status_flags_ & BP6_BundleStatusReport::STATUS_DELETED)
            {
                if (rpt_text.length() > 0) rpt_text.append(" & ");
                rpt_text.append("DELETED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%" PRIu64, sr_data.deletion_tv_.seconds_);
                rpt_text.append(tmptxt);
            }

            log_info_p("/statusrpt", "Report from %s: Bundle %s status(%d): %s : %s",
                       bundle->source().c_str(),
                       source_gbofid.str().c_str(),
                       sr_data.status_flags_,
                       rpt_text.c_str(),
                       BP6_BundleStatusReport::reason_to_str(sr_data.reason_code_));
            
        } else {
            log_err("Error parsing Status Report bundle: *%p", bundle);
            is_delivered = false;
        }           
        break;
    }
    
    case BundleProtocolVersion6::ADMIN_CUSTODY_SIGNAL:
    {
        log_info("ADMIN_CUSTODY_SIGNAL *%p received", bundle);
        CustodySignal::data_t data;
        
        bool ok = CustodySignal::parse_custody_signal(&data, payload_buf, payload_len);
        if (!ok) {
            log_err("malformed custody signal *%p", bundle);
            break;
        }

        BundleDaemon::post(new CustodySignalEvent(data, 0)); // Bundle ID will be filled in later

        break;
    }

    case BundleProtocolVersion6::ADMIN_AGGREGATE_CUSTODY_SIGNAL:
    {
        log_info("ADMIN_AGGREGATE_CUSTODY_SIGNAL *%p received", bundle);
        AggregateCustodySignal::data_t data;
        
        bool ok = AggregateCustodySignal::parse_aggregate_custody_signal(&data, payload_buf, payload_len);
        if (!ok) {
            log_err("malformed aggregate custody signal *%p", bundle);

            // delete the entry map which was allocated in 
            // AggregateCustodySignal::parse_aggregate_custody_signal()
            delete data.acs_entry_map_;

            break;
        }
        std::string dest("bpv6");
        BundleDaemon::post(new AggregateCustodySignalEvent(dest, data));

        BundleDaemon::post(new ExternalRouterAcsEvent((const char*)payload_buf, payload_len));
        break;
    }

    case BundleProtocolVersion6::ADMIN_ANNOUNCE:
    {
        log_info("ADMIN_ANNOUNCE from %s", bundle->source().c_str());
        break;
    }
        
    case BundleProtocolVersion6::ADMIN_BUNDLE_IN_BUNDLE_ENCAP:
    {
        log_info("BP6 ADMIN_BUNDLE_IN_BUNDLE_ENCAP *%p received", bundle);

        BundleDaemon::instance()->bibe_extractor_post(bundle, this);
        return;
        break;
    }

    default:
        log_warn("unexpected admin bundle with type 0x%x *%p",
                 typecode, bundle);
        is_delivered = false;
    }    


 done:
    // Flag Admin bundles as delivered
    if (is_delivered) {
        bundle->fwdlog()->update(this, ForwardingInfo::DELIVERED);
    }

    BundleDaemon::post(new BundleDeliveredEvent(bundle, this));
}

void
AdminRegistrationIpn::deliver_bundle_bp7(Bundle* bundle)
{
    int status = 0;
    uint64_t admin_type = 0;

    CborUtilBP7 cborutil("adminregipn");


    size_t payload_len = bundle->payload().length();
    oasys::ScratchBuffer<u_char*, 256> scratch(payload_len);
    const uint8_t* payload_buf = 
        bundle->payload().read_data(0, payload_len, scratch.buf(payload_len));

    bool is_delivered = true;

    log_debug("got %zu byte bundle", payload_len);
    // Admin payloads are a 2 element array with first element the type

    if (payload_len == 0) {
        log_err("admin registration got 0 byte *%p", bundle);
        //is_delivered = false;
        goto done;
    }


    if (!bundle->is_admin()) {
        if (nullptr != strstr((const char*)payload_buf, "ping"))  {
            Bundle* reply = new Bundle();
            reply->set_source(endpoint_);
            reply->set_bp_version(bundle->bp_version());
            reply->mutable_dest()->assign(bundle->source());
            reply->mutable_replyto()->assign(EndpointID::NULL_EID());
            reply->mutable_custodian()->assign(EndpointID::NULL_EID());
            reply->set_expiration_millis(bundle->expiration_millis());

            reply->mutable_payload()->set_length(payload_len);
            reply->mutable_payload()->write_data(bundle->payload(), 0, payload_len, 0);

            BundleDaemon::post(new BundleReceivedEvent(reply, EVENTSRC_ADMIN));
        } else {
            log_warn("non-admin *%p sent to local eid", bundle);
            is_delivered = false;
        } 
        goto done;
    }

    CborError err;
    CborParser parser;
    CborValue cvPayloadArray;
    CborValue cvPayloadElements;

    err = cbor_parser_init(payload_buf, payload_len, 0, &parser, &cvPayloadArray);
    CHECK_CBOR_DECODE_ERR

    SET_FLDNAMES("AdminRegIpn-PayloadArray");
    uint64_t block_elements;
    status = cborutil.validate_cbor_fixed_array_length(cvPayloadArray, 2, 2, block_elements);
    CHECK_CBOR_STATUS

    err = cbor_value_enter_container(&cvPayloadArray, &cvPayloadElements);
    CHECK_CBOR_DECODE_ERR


    // Admin Type
    SET_FLDNAMES("AdminRegIpn-Type");
    status = cborutil.decode_uint(cvPayloadElements, admin_type);
    CHECK_CBOR_STATUS



    /*
     * Admin Type Values:
     *
     * 0x01     - bundle status report            (draft-ietf-dtn-bpbis)
     * 0x02     - undefined
     * 0x03     - bundle in bundle encapsulation  (draft-ietf-dtn-bibect)
     * 0x04     - custodial signal                (draft-ietf-dtn-bibect)
     * 0x05     - undefined
     * (other) - reserved
     */
    
    switch(admin_type) {
    case BundleProtocolVersion7::ADMIN_STATUS_REPORT:
    {
        BundleStatusReport::data_t sr_data;
        if (BundleStatusReport::parse_status_report(cvPayloadElements, cborutil, sr_data))
        {
           GbofId source_gbofid(sr_data.orig_source_eid_,
                                sr_data.orig_creation_ts_,
                                (sr_data.orig_frag_length_ > 0),
                                sr_data.orig_frag_length_,
                                sr_data.orig_frag_offset_);

            char tmptxt[32];
            std::string rpt_text;
            if (sr_data.received_)
            {
                rpt_text.append("RECEIVED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%"  PRIu64, sr_data.received_timestamp_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.forwarded_)
            {
                rpt_text.append("FORWARDED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%"  PRIu64, sr_data.forwarded_timestamp_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.delivered_)
            {
                rpt_text.append("DELIVERED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%"  PRIu64, sr_data.delivered_timestamp_);
                rpt_text.append(tmptxt);
            }
            if (sr_data.deleted_)
            {
                rpt_text.append("DELETED at ");
                snprintf(tmptxt, sizeof(tmptxt), "%"  PRIu64, sr_data.deleted_timestamp_);
                rpt_text.append(tmptxt);
            }

            log_info_p("/statusrpt", "Report from %s: Bundle %s status: %s : %s",
                       bundle->source().c_str(),
                       source_gbofid.str().c_str(),
                       rpt_text.c_str(),
                       BundleStatusReport::reason_to_str(sr_data.reason_code_));
            
        } else {
            log_err("Error parsing Status Report bundle: *%p", bundle);
        }           
        break;
    }
    
    case BundleProtocolVersion7::ADMIN_CUSTODY_SIGNAL:
    {
        log_info("BP7 (BIBE) ADMIN_CUSTODY_SIGNAL *%p received", bundle);
        AggregateCustodySignal::data_t data;
        
        bool ok = AggregateCustodySignal::parse_bibe_custody_signal(cvPayloadElements, cborutil, &data);
        if (!ok) {
            log_err("malformed BIBE custody signal *%p", bundle);

            delete data.acs_entry_map_;
            is_delivered = false;
            break;
        }
        BundleDaemon::post(new AggregateCustodySignalEvent(bundle->source().str(), data));
        break;
    }

    case BundleProtocolVersion7::ADMIN_BUNDLE_IN_BUNDLE_ENCAP:
    {
        log_info("ADMIN_BUNDLE_IN_BUNDLE_ENCAP *%p received", bundle);
       
        BundleDaemon::instance()->bibe_extractor_post(bundle, this);
        return;
        break;
    }

    default:
        log_warn("unexpected admin bundle with type %" PRIu64 " *%p",
                 admin_type, bundle);
        is_delivered = false;
    }    


 done:
    // Flag Admin bundles as delivered
    if (is_delivered) {
        bundle->fwdlog()->update(this, ForwardingInfo::DELIVERED);
    }

    BundleDaemon::post(new BundleDeliveredEvent(bundle, this));
}



} // namespace dtn
