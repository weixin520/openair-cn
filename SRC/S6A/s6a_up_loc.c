/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under 
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.  
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */


#include <stdio.h>
#include <stdint.h>

#include "mme_config.h"
#include "assertions.h"
#include "conversions.h"
#include "intertask_interface.h"
#include "s6a_defs.h"
#include "s6a_messages.h"
#include "msc.h"
#include "log.h"


int
s6a_ula_cb (
  struct msg **msg_pP,
  struct avp *paramavp_pP,
  struct session *sess_pP,
  void *opaque_pP,
  enum disp_action *act_pP)
{
  struct msg                             *ans_p = NULL;
  struct msg                             *qry_p = NULL;
  struct avp                             *avp_p = NULL;
  struct avp_hdr                         *hdr_p = NULL;
  MessageDef                             *message_p = NULL;
  s6a_update_location_ans_t              *s6a_update_location_ans_p = NULL;

  DevAssert (msg_pP );
  ans_p = *msg_pP;
  /*
   * Retrieve the original query associated with the asnwer
   */
  CHECK_FCT (fd_msg_answ_getq (ans_p, &qry_p));
  DevAssert (qry_p );
  message_p = itti_alloc_new_message (TASK_S6A, S6A_UPDATE_LOCATION_ANS);
  s6a_update_location_ans_p = &message_p->ittiMsg.s6a_update_location_ans;
  CHECK_FCT (fd_msg_search_avp (qry_p, s6a_fd_cnf.dataobj_s6a_user_name, &avp_p));

  if (avp_p) {
    CHECK_FCT (fd_msg_avp_hdr (avp_p, &hdr_p));
    memcpy (s6a_update_location_ans_p->imsi, hdr_p->avp_value->os.data, hdr_p->avp_value->os.len);
    s6a_update_location_ans_p->imsi[hdr_p->avp_value->os.len] = '\0';
    s6a_update_location_ans_p->imsi_length = hdr_p->avp_value->os.len;
    LOG_DEBUG (LOG_S6A, "Received s6a ula for imsi=%*s\n", (int)hdr_p->avp_value->os.len, hdr_p->avp_value->os.data);
  } else {
    DevMessage ("Query has been freed before we received the answer\n");
  }

  /*
   * Retrieve the result-code
   */
  CHECK_FCT (fd_msg_search_avp (ans_p, s6a_fd_cnf.dataobj_s6a_result_code, &avp_p));

  if (avp_p) {
    CHECK_FCT (fd_msg_avp_hdr (avp_p, &hdr_p));
    s6a_update_location_ans_p->result.present = S6A_RESULT_BASE;
    s6a_update_location_ans_p->result.choice.base = hdr_p->avp_value->u32;
    MSC_LOG_TX_MESSAGE (MSC_S6A_MME, MSC_MMEAPP_MME, NULL, 0, "0 S6A_UPDATE_LOCATION_ANS imsi %SCNu64 %s", s6a_update_location_ans_p->imsi, retcode_2_string (hdr_p->avp_value->u32));

    if (hdr_p->avp_value->u32 != ER_DIAMETER_SUCCESS) {
      LOG_ERROR (LOG_S6A, "Got error %u:%s\n", hdr_p->avp_value->u32, retcode_2_string (hdr_p->avp_value->u32));
      goto err;
    }
  } else {
    /*
     * The result-code is not present, may be it is an experimental result
     * * * * avp_p indicating a 3GPP specific failure.
     */
    CHECK_FCT (fd_msg_search_avp (ans_p, s6a_fd_cnf.dataobj_s6a_experimental_result, &avp_p));

    if (avp_p) {
      /*
       * The procedure has failed within the HSS.
       * * * * NOTE: contrary to result-code, the experimental-result is a grouped
       * * * * AVP and requires parsing its childs to get the code back.
       */
      s6a_update_location_ans_p->result.present = S6A_RESULT_EXPERIMENTAL;
      s6a_parse_experimental_result (avp_p, &s6a_update_location_ans_p->result.choice.experimental);
      goto err;
    } else {
      /*
       * Neither result-code nor experimental-result is present ->
       * * * * totally incorrect behaviour here.
       */
      LOG_ERROR (LOG_S6A, "Experimental-Result and Result-Code are absent: " "This is not a correct behaviour\n");
      goto err;
    }
  }

  /*
   * Retrieving the ULA flags
   */
  CHECK_FCT (fd_msg_search_avp (ans_p, s6a_fd_cnf.dataobj_s6a_ula_flags, &avp_p));

  if (avp_p) {
    CHECK_FCT (fd_msg_avp_hdr (avp_p, &hdr_p));

    /*
     * This bit, when set, indicates that the HSS stores SGSN number
     * * * * and MME number in separate memory. A Rel-8 HSS shall set
     * * * * the bit.
     */
    if (!FLAG_IS_SET (hdr_p->avp_value->u32, ULA_SEPARATION_IND)) {
      LOG_ERROR (LOG_S6A, "ULA-Flags does not indicate the HSS is post Rel.8: " "This behaviour is not compliant\n");
      goto err;
    }
  } else {
    /*
     * ULA-Flags is absent while the error code indicates DIAMETER_SUCCESS:
     * * * * this is not a compliant behaviour...
     * * * * TODO: handle this case.
     */
    LOG_ERROR (LOG_S6A, "ULA-Flags AVP is absent while result code indicates " "DIAMETER_SUCCESS\n");
    goto err;
  }

  CHECK_FCT (fd_msg_search_avp (ans_p, s6a_fd_cnf.dataobj_s6a_subscription_data, &avp_p));

  if (avp_p) {
    CHECK_FCT (s6a_parse_subscription_data (avp_p, &s6a_update_location_ans_p->subscription_data));
    // LG COMMENTED THIS (2014/04/01)-> DevParam(0, 0, 0);
  }

err:
  ans_p = NULL;
  itti_send_msg_to_task (TASK_MME_APP, INSTANCE_DEFAULT, message_p);
  LOG_DEBUG (LOG_S6A, "Sending S6A_UPDATE_LOCATION_ANS to task MME_APP\n");
  return RETURNok;
}



int
s6a_generate_update_location (
  s6a_update_location_req_t * ulr_pP)
{
  struct avp                             *avp_p = NULL;
  struct msg                             *msg_p = NULL;
  struct session                         *sess_p = NULL;
  union avp_value                         value;

  DevAssert (ulr_pP );
  /*
   * Create the new update location request message
   */
  CHECK_FCT (fd_msg_new (s6a_fd_cnf.dataobj_s6a_ulr, 0, &msg_p));
  /*
   * Create a new session
   */
  CHECK_FCT (fd_sess_new (&sess_p, fd_g_config->cnf_diamid, fd_g_config->cnf_diamid_len, (os0_t) "apps6a", 6));
  {
    os0_t                                   sid;
    size_t                                  sidlen;

    CHECK_FCT (fd_sess_getsid (sess_p, &sid, &sidlen));
    CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_session_id, 0, &avp_p));
    value.os.data = sid;
    value.os.len = sidlen;
    CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
    CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_FIRST_CHILD, avp_p));
  }
  CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_auth_session_state, 0, &avp_p));
  /*
   * No State maintained
   */
  value.i32 = 1;
  CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
  CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  /*
   * Add Origin_Host & Origin_Realm
   */
  CHECK_FCT (fd_msg_add_origin (msg_p, 0));
  config_read_lock (&mme_config);
  /*
   * Destination Host
   */
  {
    char                                    host[100];
    size_t                                  hostlen;

    memset (host, 0, 100);
    strcat (host, mme_config.s6a_config.hss_host_name);
    strcat (host, ".");
    strcat (host, mme_config.realm);
    hostlen = strlen (host);
    CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_destination_host, 0, &avp_p));
    value.os.data = (unsigned char *)host;
    value.os.len = hostlen;
    CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
    CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  }
  /*
   * Destination_Realm
   */
  {
    char                                   *realm = mme_config.realm;
    size_t                                  realmlen = strlen (realm);

    CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_destination_realm, 0, &avp_p));
    value.os.data = (unsigned char *)realm;
    value.os.len = realmlen;
    CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
    CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  }
  config_unlock (&mme_config);
  /*
   * Adding the User-Name (IMSI)
   */
  CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_user_name, 0, &avp_p));
  value.os.data = (unsigned char *)ulr_pP->imsi;
  value.os.len = strlen (ulr_pP->imsi);
  CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
  CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  /*
   * Adding the visited plmn id
   */
  {
    uint8_t                                 plmn[3];

    CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_visited_plmn_id, 0, &avp_p));
    PLMN_T_TO_TBCD (ulr_pP->visited_plmn,
                    plmn,
                    mme_config_find_mnc_length (ulr_pP->visited_plmn.mcc_digit1, ulr_pP->visited_plmn.mcc_digit2, ulr_pP->visited_plmn.mcc_digit3, ulr_pP->visited_plmn.mnc_digit1, ulr_pP->visited_plmn.mnc_digit2, ulr_pP->visited_plmn.mnc_digit3)
      );
    value.os.data = plmn;
    value.os.len = 3;
    CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
    CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  }
  /*
   * Adding the RAT-Type
   */
  CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_rat_type, 0, &avp_p));
  DevCheck (ulr_pP->rat_type == RAT_EUTRAN, ulr_pP->rat_type, 0, 0);
  value.u32 = ulr_pP->rat_type;
  CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
  CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  /*
   * Adding ULR-Flags
   */
  CHECK_FCT (fd_msg_avp_new (s6a_fd_cnf.dataobj_s6a_ulr_flags, 0, &avp_p));
  value.u32 = 0;
  /*
   * Identify the ULR as coming from S6A interface (i.e. from MME)
   */
  FLAGS_SET (value.u32, ULR_S6A_S6D_INDICATOR);

  /*
   * Set the ulr-flags as indicated by upper layer
   */
  if (ulr_pP->skip_subscriber_data) {
    FLAGS_SET (value.u32, ULR_SKIP_SUBSCRIBER_DATA);
  }

  if (ulr_pP->initial_attach) {
    FLAGS_SET (value.u32, ULR_INITIAL_ATTACH_IND);
  }

  CHECK_FCT (fd_msg_avp_setvalue (avp_p, &value));
  CHECK_FCT (fd_msg_avp_add (msg_p, MSG_BRW_LAST_CHILD, avp_p));
  CHECK_FCT (fd_msg_send (&msg_p, NULL, NULL));
  LOG_DEBUG (LOG_S6A, "Sending s6a ulr for imsi=%s\n", ulr_pP->imsi);
  return RETURNok;
}
