// Copyright (c) 2014-2022, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <unordered_set>
#include <random>
#include "include_base_utils.h"
#include "string_tools.h"
using namespace epee;

#include "common/apply_permutation.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_config.h"
#include "blockchain.h"
#include "cryptonote_basic/miner.h"
#include "cryptonote_basic/tx_extra.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "ringct/rctSigs.h"
#include "multisig/multisig.h"
#include "offshore/asset_types.h"

#include <boost/multiprecision/cpp_bin_float.hpp>

using namespace crypto;

namespace cryptonote
{
  //---------------------------------------------------------------
  void classify_addresses(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr, size_t &num_stdaddresses, size_t &num_subaddresses, account_public_address &single_dest_subaddress)
  {
    num_stdaddresses = 0;
    num_subaddresses = 0;
    std::unordered_set<cryptonote::account_public_address> unique_dst_addresses;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      if (change_addr && dst_entr.addr == change_addr)
        continue;
      if (unique_dst_addresses.count(dst_entr.addr) == 0)
      {
        unique_dst_addresses.insert(dst_entr.addr);
        if (dst_entr.is_subaddress)
        {
          ++num_subaddresses;
          single_dest_subaddress = dst_entr.addr;
        }
        else
        {
          ++num_stdaddresses;
        }
      }
    }
    LOG_PRINT_L2("destinations include " << num_stdaddresses << " standard addresses and " << num_subaddresses << " subaddresses");
  }
  //---------------------------------------------------------------
  bool construct_miner_tx(size_t height, size_t median_weight, uint64_t already_generated_coins, size_t current_block_weight, uint64_t fee, const account_public_address &miner_address, transaction& tx, const blobdata& extra_nonce, size_t max_outs, uint8_t hard_fork_version) {
    tx.vin.clear();
    tx.vout.clear();
    tx.extra.clear();

    keypair txkey = keypair::generate(hw::get_device("default"));
    add_tx_pub_key_to_extra(tx, txkey.pub);
    if(!extra_nonce.empty())
      if(!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
        return false;
    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    txin_gen in;
    in.height = height;

    uint64_t block_reward;
    if(!get_block_reward(median_weight, current_block_weight, already_generated_coins, block_reward, hard_fork_version))
    {
      LOG_PRINT_L0("Block is too big");
      return false;
    }

#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
    LOG_PRINT_L1("Creating block template: reward " << block_reward <<
      ", fee " << fee);
#endif
    block_reward += fee;

    // from hard fork 2, we cut out the low significant digits. This makes the tx smaller, and
    // keeps the paid amount almost the same. The unpaid remainder gets pushed back to the
    // emission schedule
    // from hard fork 4, we use a single "dusty" output. This makes the tx even smaller,
    // and avoids the quantization. These outputs will be added as rct outputs with identity
    // masks, to they can be used as rct inputs.
    if (hard_fork_version >= 2 && hard_fork_version < 4) {
      block_reward = block_reward - block_reward % ::config::BASE_REWARD_CLAMP_THRESHOLD;
    }

    std::vector<uint64_t> out_amounts;
    decompose_amount_into_digits(block_reward, hard_fork_version >= 2 ? 0 : ::config::DEFAULT_DUST_THRESHOLD,
      [&out_amounts](uint64_t a_chunk) { out_amounts.push_back(a_chunk); },
      [&out_amounts](uint64_t a_dust) { out_amounts.push_back(a_dust); });

    CHECK_AND_ASSERT_MES(1 <= max_outs, false, "max_out must be non-zero");
    if (height == 0 || hard_fork_version >= 4)
    {
      // the genesis block was not decomposed, for unknown reasons
      while (max_outs < out_amounts.size())
      {
        //out_amounts[out_amounts.size() - 2] += out_amounts.back();
        //out_amounts.resize(out_amounts.size() - 1);
        out_amounts[1] += out_amounts[0];
        for (size_t n = 1; n < out_amounts.size(); ++n)
          out_amounts[n - 1] = out_amounts[n];
        out_amounts.pop_back();
      }
    }
    else
    {
      CHECK_AND_ASSERT_MES(max_outs >= out_amounts.size(), false, "max_out exceeded");
    }

    uint64_t summary_amounts = 0;
    for (size_t no = 0; no < out_amounts.size(); no++)
    {
      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
      bool r = crypto::generate_key_derivation(miner_address.m_view_public_key, txkey.sec, derivation);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << miner_address.m_view_public_key << ", " << txkey.sec << ")");

      r = crypto::derive_public_key(derivation, no, miner_address.m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << no << ", "<< miner_address.m_spend_public_key << ")");

      uint64_t amount = out_amounts[no];
      summary_amounts += amount;

      bool use_view_tags = hard_fork_version >= HF_VERSION_VIEW_TAGS;
      crypto::view_tag view_tag;
      if (use_view_tags)
        crypto::derive_view_tag(derivation, no, view_tag);

      tx_out out;
      cryptonote::set_tx_out(amount, out_eph_public_key, use_view_tags, view_tag, out);

      tx.vout.push_back(out);
    }

    CHECK_AND_ASSERT_MES(summary_amounts == block_reward, false, "Failed to construct miner tx, summary_amounts = " << summary_amounts << " not equal block_reward = " << block_reward);

    if (hard_fork_version >= 4)
      tx.version = 2;
    else
      tx.version = 1;

    //lock
    tx.unlock_time = height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;
    tx.vin.push_back(in);

    tx.invalidate_hashes();

    //LOG_PRINT("MINER_TX generated ok, block_reward=" << print_money(block_reward) << "("  << print_money(block_reward - fee) << "+" << print_money(fee)
    //  << "), current_block_size=" << current_block_size << ", already_generated_coins=" << already_generated_coins << ", tx_id=" << get_transaction_hash(tx), LOG_LEVEL_2);
    return true;
  }
  //---------------------------------------------------------------
  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr)
  {
    account_public_address addr = {null_pkey, null_pkey};
    size_t count = 0;
    for (const auto &i : destinations)
    {
      if (i.amount == 0)
        continue;
      if (change_addr && i.addr == *change_addr)
        continue;
      if (i.addr == addr)
        continue;
      if (count > 0)
        return null_pkey;
      addr = i.addr;
      ++count;
    }
    if (count == 0 && change_addr)
      return change_addr->m_view_public_key;
    return addr.m_view_public_key;
  }
  //---------------------------------------------------------------
  uint64_t get_offshore_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t unlock_time, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      if (dt.amount_usd != 0 && !dt.is_collateral) {
        amount += dt.amount;
      }
    }

    uint64_t fee_estimate = 0;
    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Flat 1.5% fee
      fee_estimate = (amount * 3) / 200;
    } else if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      // Flat 0.5% fee
      fee_estimate = amount / 200;
    } else {
      // The tests have to be written largest unlock_time first, as it is possible to delay the construction of the TX using GDB etc
      // which would otherwise cause the umlock_time to fall through the gaps and give a minimum fee for a short unlock_time.
      // This way, the code is safe, and the fee is always correct.
      fee_estimate =
      (unlock_time >= 5040) ? (amount / 500) :
      (unlock_time >= 1440) ? (amount / 20) :
      (unlock_time >= 720) ? (amount / 10) :
      amount / 5;
    }

    return fee_estimate;
  }
  //---------------------------------------------------------------
  uint64_t get_onshore_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t unlock_time, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount_usd = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      if (dt.amount != 0 && !dt.is_collateral) {
        amount_usd += dt.amount_usd;
      }
    }

    uint64_t fee_estimate = 0;
     if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Flat 1.5% fee
      fee_estimate = (amount_usd * 3) / 200;
    } else if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      // Flat 0.5% fee
      fee_estimate = amount_usd / 200;
    } else {
      // The tests have to be written largest unlock_time first, as it is possible to delay the construction of the TX using GDB etc
      // which would otherwise cause the umlock_time to fall through the gaps and give a minimum fee for a short unlock_time.
      // This way, the code is safe, and the fee is always correct.
      fee_estimate =
      (unlock_time >= 5040) ? (amount_usd / 500) :
      (unlock_time >= 1440) ? (amount_usd / 20) :
      (unlock_time >= 720) ? (amount_usd / 10) :
      amount_usd / 5;

    }

    return fee_estimate;
  }
  //---------------------------------------------------------------
  uint64_t get_xasset_to_xusd_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount_xasset = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      if (dt.amount_usd != 0) {
        amount_xasset += dt.amount_xasset;
      }
    }

    uint64_t fee_estimate = 0;  
    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Calculate 1.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_xasset;
      amount_128 = (amount_128 * 15) / 1000; // 1.5%
      fee_estimate  = (uint64_t)amount_128;
    } else if (hf_version >= HF_VERSION_XASSET_FEES_V2) {
      // Calculate 0.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_xasset;
      amount_128 = (amount_128 * 5) / 1000; // 0.5%
      fee_estimate  = (uint64_t)amount_128;
    } else {
      // Calculate 0.3% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_xasset;
      amount_128 = (amount_128 * 3) / 1000;
      fee_estimate = (uint64_t)amount_128;
    }

   return fee_estimate;
  }
  //---------------------------------------------------------------
  uint64_t get_xusd_to_xasset_fee(const std::vector<cryptonote::tx_destination_entry>& dsts, const uint32_t hf_version) {

    // Calculate the amount being sent
    uint64_t amount_usd = 0;
    for (auto dt: dsts) {
      // Filter out the change, which is never converted
      // All other destinations should have both pre and post converted amounts set so far except
      // the change destinations.
      if (dt.amount_xasset != 0) {
        amount_usd += dt.amount_usd;
      }
    }

    uint64_t fee_estimate = 0;
    if (hf_version >= HF_VERSION_USE_COLLATERAL) {
      // Calculate 1.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_usd;
      amount_128 = (amount_128 * 15) / 1000; // 1.5%
      fee_estimate  = (uint64_t)amount_128;
    } else if (hf_version >= HF_VERSION_XASSET_FEES_V2) {
      // Calculate 0.5% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_usd;
      amount_128 = (amount_128 * 5) / 1000; // 0.5%
      fee_estimate  = (uint64_t)amount_128;
    } else {
      // Calculate 0.3% of the total being sent
      boost::multiprecision::uint128_t amount_128 = amount_usd;
      amount_128 = (amount_128 * 3) / 1000;
      fee_estimate = (uint64_t)amount_128;
    }

    return fee_estimate;
  }
  //---------------------------------------------------------------
  bool get_tx_asset_types(const transaction& tx, const crypto::hash &txid, std::string& source, std::string& destination, const bool is_miner_tx) {

    // Clear the source
    std::set<std::string> source_asset_types;
    source = "";
    for (size_t i = 0; i < tx.vin.size(); i++) {
      if (tx.vin[i].type() == typeid(txin_gen)) {
        if (!is_miner_tx) {
          LOG_ERROR("txin_gen detected in non-miner TX. Rejecting..");
          return false;
        }
        source_asset_types.insert("XHV");
      } else if (tx.vin[i].type() == typeid(txin_to_key)) {
        source_asset_types.insert("XHV");
      } else if (tx.vin[i].type() == typeid(txin_offshore)) {
        source_asset_types.insert("XUSD");
      } else if (tx.vin[i].type() == typeid(txin_onshore)) {
        source_asset_types.insert("XUSD");
      } else if (tx.vin[i].type() == typeid(txin_xasset)) {
        std::string xasset = boost::get<txin_xasset>(tx.vin[i]).asset_type;
        if (xasset == "XHV" || xasset == "XUSD") {
          LOG_ERROR("XHV or XUSD found in a xasset input. Rejecting..");
          return false;
        }
        source_asset_types.insert(xasset);
      } else {
        LOG_ERROR("txin_to_script / txin_to_scripthash detected. Rejecting..");
        return false;
      }
    }

    std::vector<std::string> sat;
    sat.reserve(source_asset_types.size());
    std::copy(source_asset_types.begin(), source_asset_types.end(), std::back_inserter(sat));
    
    // Sanity check that we only have 1 source asset type
    if (tx.version >= COLLATERAL_TRANSACTION_VERSION && sat.size() == 2) {
      // this is only possible for an onshore tx.
      if ((sat[0] == "XHV" && sat[1] == "XUSD") || (sat[0] == "XUSD" && sat[1] == "XHV")) {
        source = "XUSD";
      } else {
        LOG_ERROR("Impossible input asset types. Rejecting..");
        return false;
      }
    } else {
      if (sat.size() != 1) {
        LOG_ERROR("Multiple Source Asset types detected. Rejecting..");
        return false;
      }
      source = sat[0];
    }
    
    // Clear the destination
    std::set<std::string> destination_asset_types;
    destination = "";
    for (const auto &out: tx.vout) {
      if (out.target.type() == typeid(txout_to_key)) {
        destination_asset_types.insert("XHV");
      } else if (out.target.type() == typeid(txout_offshore)) {
        destination_asset_types.insert("XUSD");
      } else if (out.target.type() == typeid(txout_xasset)) {
        std::string xasset = boost::get<txout_xasset>(out.target).asset_type;
        if (xasset == "XHV" || xasset == "XUSD") {
          LOG_ERROR("XHV or XUSD found in a xasset output. Rejecting..");
          return false;
        }
        destination_asset_types.insert(xasset);
      } else {
        LOG_ERROR("txout_to_script / txout_to_scripthash detected. Rejecting..");
        return false;
      }
    }

    std::vector<std::string> dat;
    dat.reserve(destination_asset_types.size());
    std::copy(destination_asset_types.begin(), destination_asset_types.end(), std::back_inserter(dat));
    
    // Check that we have at least 1 destination_asset_type
    if (!dat.size()) {
      LOG_ERROR("No supported destinations asset types detected. Rejecting..");
      return false;
    }
    
    // Handle miner_txs differently - full validation is performed in validate_miner_transaction()
    if (is_miner_tx) {
      destination = "XHV";
    } else {
    
      // Sanity check that we only have 1 or 2 destination asset types
      if (dat.size() > 2) {
        LOG_ERROR("Too many (" << dat.size() << ") destination asset types detected in non-miner TX. Rejecting..");
        return false;
      } else if (dat.size() == 1) {
        if (sat.size() != 1) {
          LOG_ERROR("Impossible input asset types. Rejecting..");
          return false;
        }
        if (dat[0] != source) {
          LOG_ERROR("Conversion without change detected ([" << source << "] -> [" << dat[0] << "]). Rejecting..");
          return false;
        }
        destination = dat[0];
      } else {
        if (sat.size() == 2) {
          if (!((dat[0] == "XHV" && dat[1] == "XUSD") || (dat[0] == "XUSD" && dat[1] == "XHV"))) {
            LOG_ERROR("Impossible input asset types. Rejecting..");
            return false;
          }
        }
        if (dat[0] == source) {
          destination = dat[1];
        } else if (dat[1] == source) {
          destination = dat[0];
        } else {
          LOG_ERROR("Conversion outputs are incorrect asset types (source asset type not found - [" << source << "] -> [" << dat[0] << "," << dat[1] << "]). Rejecting..");
          return false;
        }
      }
    }
    
    // check both strSource and strDest are supported.
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), source) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Source Asset type " << source << " is not supported! Rejecting..");
      return false;
    }
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), destination) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Destination Asset type " << destination << " is not supported! Rejecting..");
      return false;
    }

    // Check for the 3 known exploited TXs that converted XJPY to XBTC
    const std::vector<std::string> exploit_txs = {"4c87e7245142cb33a8ed4f039b7f33d4e4dd6b541a42a55992fd88efeefc40d1",
                                                  "7089a8faf5bddf8640a3cb41338f1ec2cdd063b1622e3b27923e2c1c31c55418",
                                                  "ad5d15085594b8f2643f058b05931c3e60966128b4c33298206e70bdf9d41c22"};

    std::string tx_hash = epee::string_tools::pod_to_hex(txid);
    if (std::find(exploit_txs.begin(), exploit_txs.end(), tx_hash) != exploit_txs.end()) {
      destination = "XJPY";
    }
    return true;
  }
  //---------------------------------------------------------------
  bool get_tx_type(const std::string& source, const std::string& destination, transaction_type& type) {

    // check both source and destination are supported.
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), source) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Source Asset type " << source << " is not supported! Rejecting..");
      return false;
    }
    if (std::find(offshore::ASSET_TYPES.begin(), offshore::ASSET_TYPES.end(), destination) == offshore::ASSET_TYPES.end()) {
      LOG_ERROR("Destination Asset type " << destination << " is not supported! Rejecting..");
      return false;
    }

    // Find the tx type
    if (source == destination) {
      if (source == "XHV") {
        type = transaction_type::TRANSFER;
      } else if (source == "XUSD") {
        type = transaction_type::OFFSHORE_TRANSFER;
      } else {
        type = transaction_type::XASSET_TRANSFER;
      }
    } else {
      if (source == "XHV" && destination == "XUSD") {
        type = transaction_type::OFFSHORE;
      } else if (source == "XUSD" && destination == "XHV") {
        type = transaction_type::ONSHORE;
      } else if (source == "XUSD" && destination != "XHV") {
        type = transaction_type::XUSD_TO_XASSET;
      } else if (destination == "XUSD" && source != "XHV") {
        type = transaction_type::XASSET_TO_XUSD;
      } else {
        LOG_ERROR("Invalid conversion from " << source << "to" << destination << ". Rejecting..");
        return false;
      }
    }

    // Return success to caller
    return true;
  }
  //---------------------------------------------------------------
  bool get_collateral_requirements(const transaction_type &tx_type, const uint64_t amount, uint64_t &collateral, const offshore::pricing_record &pr, const std::vector<std::pair<std::string, std::string>> &amounts)
  {
    using namespace boost::multiprecision;
    using tt = transaction_type;

    // Process the circulating supply data
    std::map<std::string, uint128_t> map_amounts;
    uint128_t mcap_xassets = 0;
    for (const auto &i: amounts)
    {
      // Copy into the map for expediency
      map_amounts[i.first] = uint128_t(i.second.c_str());
      
      // Skip XHV
      if (i.first == "XHV") continue;

      // Get the pricing data for the xAsset
      uint128_t price_xasset = pr[i.first];
      
      // Multiply by the amount of coin in circulation
      uint128_t amount_xasset(i.second.c_str());
      amount_xasset *= COIN;
      amount_xasset /= price_xasset;
      
      // Sum into our total for all xAssets
      mcap_xassets += amount_xasset;
    }

    // Calculate the XHV market cap
    boost::multiprecision::uint128_t price_xhv =
      (tx_type == tt::OFFSHORE) ? std::min(pr.unused1, pr.xUSD) :
      (tx_type == tt::ONSHORE)  ? std::max(pr.unused1, pr.xUSD) :
      0;
    uint128_t mcap_xhv = map_amounts["XHV"];
    mcap_xhv *= price_xhv;
    mcap_xhv /= COIN;

    // Calculate the market cap ratio
    cpp_bin_float_quad ratio_mcap_128 = mcap_xassets.convert_to<cpp_bin_float_quad>() / mcap_xhv.convert_to<cpp_bin_float_quad>();
    double ratio_mcap = ratio_mcap_128.convert_to<double>();

    // Calculate the spread ratio
    double ratio_spread = (ratio_mcap >= 1.0) ? 0.0 : 1.0 - ratio_mcap;
    
    // Calculate the MCAP VBS rate
    double rate_mcvbs = (ratio_mcap == 0) ? 0 : (ratio_mcap < 0.9) // Fix for "possible" 0 ratio
      ? std::exp((ratio_mcap + std::sqrt(ratio_mcap))*2.0) - 0.5 // Lower MCAP ratio
      : std::sqrt(ratio_mcap) * 40.0; // Higher MCAP ratio

    // Calculate the Spread Ratio VBS rate
    double rate_srvbs = std::exp(1 + std::sqrt(ratio_spread)) + rate_mcvbs + 1.5;
    
    // Set the Slippage Multiplier
    double slippage_multiplier = 10.0;

    // Convert amount to 128 bit
    boost::multiprecision::uint128_t amount_128 = amount;
  
    // Do the right thing based upon TX type
    using tt = cryptonote::transaction_type;
    if (tx_type == tt::TRANSFER || tx_type == tt::OFFSHORE_TRANSFER || tx_type == tt::XASSET_TRANSFER) {
      collateral = 0;
    } else if (tx_type == tt::OFFSHORE) {

      // Calculate MCRI
      boost::multiprecision::uint128_t amount_usd_128 = amount;
      amount_usd_128 *= price_xhv;
      amount_usd_128 /= COIN;
      cpp_bin_float_quad ratio_mcap_new_quad = ((amount_usd_128.convert_to<cpp_bin_float_quad>() + mcap_xassets.convert_to<cpp_bin_float_quad>()) /
						(mcap_xhv.convert_to<cpp_bin_float_quad>() - amount_usd_128.convert_to<cpp_bin_float_quad>()));
      double ratio_mcap_new = ratio_mcap_new_quad.convert_to<double>();
      double ratio_mcri = (ratio_mcap == 0.0) ? ratio_mcap_new : (ratio_mcap_new / ratio_mcap) - 1.0;
      ratio_mcri = std::abs(ratio_mcri);

      // Calculate Offshore Slippage VBS rate
      if (ratio_mcap_new <= 0.1) slippage_multiplier = 3.0;
      double rate_offsvbs = std::sqrt(ratio_mcri) * slippage_multiplier;

      // Calculate the combined VBS (collateral + "slippage")
      double vbs = rate_mcvbs + rate_offsvbs;
      const double min_vbs = 1.0;
      vbs = std::max(vbs, min_vbs);
      vbs *= COIN;
      boost::multiprecision::uint128_t collateral_128 = static_cast<uint64_t>(vbs);
      collateral_128 *= amount_128;
      collateral_128 /= COIN;
      collateral = collateral_128.convert_to<uint64_t>();

      LOG_PRINT_L1("Offshore TX requires " << print_money(collateral) << " XHV as collateral to convert " << print_money(amount) << " XHV");
    
    } else if (tx_type == tt::ONSHORE) {

      // Calculate SRI
      cpp_bin_float_quad ratio_mcap_new_quad = ((mcap_xassets.convert_to<cpp_bin_float_quad>() - amount_128.convert_to<cpp_bin_float_quad>()) /
						(mcap_xhv.convert_to<cpp_bin_float_quad>() + amount_128.convert_to<cpp_bin_float_quad>()));
      double ratio_mcap_new = ratio_mcap_new_quad.convert_to<double>();
      double ratio_sri = (ratio_mcap == 0.0) ? (-1.0 * ratio_mcap_new) : ((1.0 - ratio_mcap_new) / (1.0 - ratio_mcap)) - 1.0;
      ratio_sri = std::max(ratio_sri, 0.0);
      
      // Calculate ONSVBS
      //if (ratio_mcap_new <= 0.1) slippage_multiplier = 3.0;
      //double rate_onsvbs = std::sqrt(ratio_sri) * slippage_multiplier;
      double rate_onsvbs = std::sqrt(ratio_sri) * 3.0;
  
      // Calculate the combined VBS (collateral + "slippage")
      double vbs = std::max(rate_mcvbs, rate_srvbs) + rate_onsvbs;
      const double min_vbs = 1.0;
      vbs = std::max(vbs, min_vbs);
      vbs *= COIN;
      boost::multiprecision::uint128_t collateral_128 = static_cast<uint64_t>(vbs);
      collateral_128 *= amount_128;
      collateral_128 /= price_xhv;
      collateral = collateral_128.convert_to<uint64_t>();

      boost::multiprecision::uint128_t amount_usd_128 = amount;
      amount_usd_128 *= price_xhv;
      amount_usd_128 /= COIN;
      LOG_PRINT_L1("Onshore TX requires " << print_money(collateral) << " XHV as collateral to convert " << print_money((uint64_t)amount_128) << " xUSD");
    
    } else if (tx_type == tt::XUSD_TO_XASSET || tx_type == tt::XASSET_TO_XUSD) {
      collateral = 0;
    } else {
      // Throw a wallet exception - should never happen
      MERROR("Invalid TX type");
      return false;
    }

    return true;
  }
  //---------------------------------------------------------------
  uint64_t get_block_cap(const std::vector<std::pair<std::string, std::string>>& supply_amounts, const offshore::pricing_record& pr)
  {
    std::string str_xhv_supply;
    for (const auto& supply: supply_amounts) {
      if (supply.first == "XHV") {
        str_xhv_supply = supply.second;
        break;
      }
    }

    // get supply
    boost::multiprecision::uint128_t xhv_supply_128(str_xhv_supply);
    xhv_supply_128 /= COIN;
    uint64_t xhv_supply = xhv_supply_128.convert_to<uint64_t>();

    // get price
    double price = (double)(std::min(pr.unused1, pr.xUSD)); // smaller of the ma vs spot
    price /= COIN;

    // market cap
    uint64_t xhv_market_cap = xhv_supply * price;
    
    return (pow(xhv_market_cap * 3000, 0.42) + ((xhv_supply * 5) / 1000)) * COIN;
  }
  //---------------------------------------------------------------
  uint64_t get_xasset_amount(const uint64_t xusd_amount, const std::string& to_asset_type, const offshore::pricing_record& pr)
  {
    boost::multiprecision::uint128_t xusd_128 = xusd_amount;
    boost::multiprecision::uint128_t exchange_128 = pr[to_asset_type]; 
    // Now work out the amount
    boost::multiprecision::uint128_t xasset_128 = xusd_128 * exchange_128;
    xasset_128 /= 1000000000000;

    return (uint64_t)xasset_128;
  }
  //---------------------------------------------------------------
  uint64_t get_xusd_amount(const uint64_t amount, const std::string& amount_asset_type, const offshore::pricing_record& pr, const transaction_type tx_type, uint32_t hf_version)
  {

    if (amount_asset_type == "XUSD") {
      return amount;
    }

    boost::multiprecision::uint128_t amount_128 = amount;
    boost::multiprecision::uint128_t exchange_128 = pr[amount_asset_type];
    if (amount_asset_type == "XHV") {
      // xhv -> xusd
      if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
        if (tx_type == transaction_type::ONSHORE) {
          // Eliminate MA/spot advantage for onshore conversion
          exchange_128 = std::max(pr.unused1, pr.xUSD);
        } else {
          // Eliminate MA/spot advantage for offshore conversion
          exchange_128 = std::min(pr.unused1, pr.xUSD);
        }
      }
      boost::multiprecision::uint128_t xusd_128 = amount_128 * exchange_128;
      xusd_128 /= 1000000000000;
      return (uint64_t)xusd_128;
    } else {
      // xasset -> xusd
      boost::multiprecision::uint128_t xusd_128 = amount_128 * 1000000000000;
      xusd_128 /= exchange_128;
      return (uint64_t)xusd_128;
    }
  }
  //---------------------------------------------------------------
  uint64_t get_xhv_amount(const uint64_t xusd_amount, const offshore::pricing_record& pr, const transaction_type tx_type, uint32_t hf_version)
  {
    // Now work out the amount
    boost::multiprecision::uint128_t xusd_128 = xusd_amount;
    boost::multiprecision::uint128_t exchange_128 = pr.unused1;
    boost::multiprecision::uint128_t xhv_128 = xusd_128 * 1000000000000;
    if (hf_version >= HF_PER_OUTPUT_UNLOCK_VERSION) {
      if (tx_type == transaction_type::ONSHORE) {
        // Eliminate MA/spot advantage for onshore conversion
        exchange_128 = std::max(pr.unused1, pr.xUSD);
      } else {
        // Eliminate MA/spot advantage for offshore conversion
        exchange_128 = std::min(pr.unused1, pr.xUSD);
      }
    }
    xhv_128 /= exchange_128;
    return (uint64_t)xhv_128;
  }
  //----------------------------------------------------------------------------------------------------
  bool tx_pr_height_valid(const uint64_t current_height, const uint64_t pr_height, const crypto::hash& tx_hash) {
    if (pr_height >= current_height) {
      return false;
    }
    if ((current_height - PRICING_RECORD_VALID_BLOCKS) > pr_height) {
      // exception for 1 tx that used 11 block old record and is already in the chain.
      if (epee::string_tools::pod_to_hex(tx_hash) != "3e61439c9f751a56777a1df1479ce70311755b9d42db5bcbbd873c6f09a020a6") {
        return false;
      }
    }
    return true;
  }
  //---------------------------------------------------------------
  bool construct_tx_with_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key, const std::vector<crypto::secret_key> &additional_tx_keys, bool rct, const rct::RCTConfig &rct_config, bool shuffle_outs, bool use_view_tags)
  {
    hw::device &hwdev = sender_account_keys.get_device();

    if (sources.empty())
    {
      LOG_ERROR("Empty sources");
      return false;
    }

    std::vector<rct::key> amount_keys;
    tx.set_null();
    amount_keys.clear();

    tx.version = rct ? 2 : 1;
    tx.unlock_time = unlock_time;

    tx.extra = extra;
    crypto::public_key txkey_pub;

    // if we have a stealth payment id, find it and encrypt it with the tx key now
    std::vector<tx_extra_field> tx_extra_fields;
    if (parse_tx_extra(tx.extra, tx_extra_fields))
    {
      bool add_dummy_payment_id = true;
      tx_extra_nonce extra_nonce;
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash payment_id = null_hash;
        crypto::hash8 payment_id8 = null_hash8;
        if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
        {
          LOG_PRINT_L2("Encrypting payment id " << payment_id8);
          crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
          if (view_key_pub == null_pkey)
          {
            LOG_ERROR("Destinations have to have exactly one output to support encrypted payment ids");
            return false;
          }

          if (!hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key))
          {
            LOG_ERROR("Failed to encrypt payment id");
            return false;
          }

          std::string extra_nonce;
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          remove_field_from_tx_extra(tx.extra, typeid(tx_extra_nonce));
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add encrypted payment id to tx extra");
            return false;
          }
          LOG_PRINT_L1("Encrypted payment ID: " << payment_id8);
          add_dummy_payment_id = false;
        }
        else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          add_dummy_payment_id = false;
        }
      }

      // we don't add one if we've got more than the usual 1 destination plus change
      if (destinations.size() > 2)
        add_dummy_payment_id = false;

      if (add_dummy_payment_id)
      {
        // if we have neither long nor short payment id, add a dummy short one,
        // this should end up being the vast majority of txes as time goes on
        std::string extra_nonce;
        crypto::hash8 payment_id8 = null_hash8;
        crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
        if (view_key_pub == null_pkey)
        {
          LOG_ERROR("Failed to get key to encrypt dummy payment id with");
        }
        else
        {
          hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key);
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add dummy encrypted payment id to tx extra");
            // continue anyway
          }
        }
      }
    }
    else
    {
      MWARNING("Failed to parse tx extra");
      tx_extra_fields.clear();
    }

    struct input_generation_context_data
    {
      keypair in_ephemeral;
    };
    std::vector<input_generation_context_data> in_contexts;

    uint64_t summary_inputs_money = 0;
    //fill inputs
    int idx = -1;
    for(const tx_source_entry& src_entr:  sources)
    {
      ++idx;
      if(src_entr.real_output >= src_entr.outputs.size())
      {
        LOG_ERROR("real_output index (" << src_entr.real_output << ")bigger than output_keys.size()=" << src_entr.outputs.size());
        return false;
      }
      summary_inputs_money += src_entr.amount;

      //key_derivation recv_derivation;
      in_contexts.push_back(input_generation_context_data());
      keypair& in_ephemeral = in_contexts.back().in_ephemeral;
      crypto::key_image img;
      const auto& out_key = reinterpret_cast<const crypto::public_key&>(src_entr.outputs[src_entr.real_output].second.dest);
      if(!generate_key_image_helper(sender_account_keys, subaddresses, out_key, src_entr.real_out_tx_key, src_entr.real_out_additional_tx_keys, src_entr.real_output_in_tx_index, in_ephemeral,img, hwdev))
      {
        LOG_ERROR("Key image generation failed!");
        return false;
      }

      //check that derivated key is equal with real output key
      if(!(in_ephemeral.pub == src_entr.outputs[src_entr.real_output].second.dest) )
      {
        LOG_ERROR("derived public key mismatch with output public key at index " << idx << ", real out " << src_entr.real_output << "! "<< ENDL << "derived_key:"
          << string_tools::pod_to_hex(in_ephemeral.pub) << ENDL << "real output_public_key:"
          << string_tools::pod_to_hex(src_entr.outputs[src_entr.real_output].second.dest) );
        LOG_ERROR("amount " << src_entr.amount << ", rct " << src_entr.rct);
        LOG_ERROR("tx pubkey " << src_entr.real_out_tx_key << ", real_output_in_tx_index " << src_entr.real_output_in_tx_index);
        return false;
      }

      //put key image into tx input
      txin_to_key input_to_key;
      input_to_key.amount = src_entr.amount;
      input_to_key.k_image = img;

      //fill outputs array and use relative offsets
      for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
        input_to_key.key_offsets.push_back(out_entry.first);

      input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
      tx.vin.push_back(input_to_key);
    }

    if (shuffle_outs)
    {
      std::shuffle(destinations.begin(), destinations.end(), crypto::random_device{});
    }

    // sort ins by their key image
    std::vector<size_t> ins_order(sources.size());
    for (size_t n = 0; n < sources.size(); ++n)
      ins_order[n] = n;
    std::sort(ins_order.begin(), ins_order.end(), [&](const size_t i0, const size_t i1) {
      const txin_to_key &tk0 = boost::get<txin_to_key>(tx.vin[i0]);
      const txin_to_key &tk1 = boost::get<txin_to_key>(tx.vin[i1]);
      return memcmp(&tk0.k_image, &tk1.k_image, sizeof(tk0.k_image)) > 0;
    });
    tools::apply_permutation(ins_order, [&] (size_t i0, size_t i1) {
      std::swap(tx.vin[i0], tx.vin[i1]);
      std::swap(in_contexts[i0], in_contexts[i1]);
      std::swap(sources[i0], sources[i1]);
    });

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress;
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);

    // if this is a single-destination transfer to a subaddress, we set the tx pubkey to R=s*D
    if (num_stdaddresses == 0 && num_subaddresses == 1)
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(single_dest_subaddress.m_spend_public_key), rct::sk2rct(tx_key)));
    }
    else
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(tx_key)));
    }
    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_pub_key));
    add_tx_pub_key_to_extra(tx, txkey_pub);

    std::vector<crypto::public_key> additional_tx_public_keys;

    // we don't need to include additional tx keys if:
    //   - all the destinations are standard addresses
    //   - there's only one destination which is a subaddress
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
      CHECK_AND_ASSERT_MES(destinations.size() == additional_tx_keys.size(), false, "Wrong amount of additional tx keys");

    uint64_t summary_outs_money = 0;
    //fill outputs
    size_t output_index = 0;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      CHECK_AND_ASSERT_MES(dst_entr.amount > 0 || tx.version > 1, false, "Destination with wrong amount: " << dst_entr.amount);
      crypto::public_key out_eph_public_key;
      crypto::view_tag view_tag;

      hwdev.generate_output_ephemeral_keys(tx.version,sender_account_keys, txkey_pub, tx_key,
                                           dst_entr, change_addr, output_index,
                                           need_additional_txkeys, additional_tx_keys,
                                           additional_tx_public_keys, amount_keys, out_eph_public_key,
                                           use_view_tags, view_tag);

      tx_out out;
      cryptonote::set_tx_out(dst_entr.amount, out_eph_public_key, use_view_tags, view_tag, out);
      tx.vout.push_back(out);
      output_index++;
      summary_outs_money += dst_entr.amount;
    }
    CHECK_AND_ASSERT_MES(additional_tx_public_keys.size() == additional_tx_keys.size(), false, "Internal error creating additional public keys");

    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_additional_pub_keys));

    LOG_PRINT_L2("tx pubkey: " << txkey_pub);
    if (need_additional_txkeys)
    {
      LOG_PRINT_L2("additional tx pubkeys: ");
      for (size_t i = 0; i < additional_tx_public_keys.size(); ++i)
        LOG_PRINT_L2(additional_tx_public_keys[i]);
      add_additional_tx_pub_keys_to_extra(tx.extra, additional_tx_public_keys);
    }

    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    //check money
    if(summary_outs_money > summary_inputs_money )
    {
      LOG_ERROR("Transaction inputs money ("<< summary_inputs_money << ") less than outputs money (" << summary_outs_money << ")");
      return false;
    }

    // check for watch only wallet
    bool zero_secret_key = true;
    for (size_t i = 0; i < sizeof(sender_account_keys.m_spend_secret_key); ++i)
      zero_secret_key &= (sender_account_keys.m_spend_secret_key.data[i] == 0);
    if (zero_secret_key)
    {
      MDEBUG("Null secret key, skipping signatures");
    }

    if (tx.version == 1)
    {
      //generate ring signatures
      crypto::hash tx_prefix_hash;
      get_transaction_prefix_hash(tx, tx_prefix_hash);

      std::stringstream ss_ring_s;
      size_t i = 0;
      for(const tx_source_entry& src_entr:  sources)
      {
        ss_ring_s << "pub_keys:" << ENDL;
        std::vector<const crypto::public_key*> keys_ptrs;
        std::vector<crypto::public_key> keys(src_entr.outputs.size());
        size_t ii = 0;
        for(const tx_source_entry::output_entry& o: src_entr.outputs)
        {
          keys[ii] = rct2pk(o.second.dest);
          keys_ptrs.push_back(&keys[ii]);
          ss_ring_s << o.second.dest << ENDL;
          ++ii;
        }

        tx.signatures.push_back(std::vector<crypto::signature>());
        std::vector<crypto::signature>& sigs = tx.signatures.back();
        sigs.resize(src_entr.outputs.size());
        if (!zero_secret_key)
          crypto::generate_ring_signature(tx_prefix_hash, boost::get<txin_to_key>(tx.vin[i]).k_image, keys_ptrs, in_contexts[i].in_ephemeral.sec, src_entr.real_output, sigs.data());
        ss_ring_s << "signatures:" << ENDL;
        std::for_each(sigs.begin(), sigs.end(), [&](const crypto::signature& s){ss_ring_s << s << ENDL;});
        ss_ring_s << "prefix_hash:" << tx_prefix_hash << ENDL << "in_ephemeral_key: " << in_contexts[i].in_ephemeral.sec << ENDL << "real_output: " << src_entr.real_output << ENDL;
        i++;
      }

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL << ss_ring_s.str());
    }
    else
    {
      size_t n_total_outs = sources[0].outputs.size(); // only for non-simple rct

      // the non-simple version is slightly smaller, but assumes all real inputs
      // are on the same index, so can only be used if there just one ring.
      bool use_simple_rct = sources.size() > 1 || rct_config.range_proof_type != rct::RangeProofBorromean;

      if (!use_simple_rct)
      {
        // non simple ringct requires all real inputs to be at the same index for all inputs
        for(const tx_source_entry& src_entr:  sources)
        {
          if(src_entr.real_output != sources.begin()->real_output)
          {
            LOG_ERROR("All inputs must have the same index for non-simple ringct");
            return false;
          }
        }

        // enforce same mixin for all outputs
        for (size_t i = 1; i < sources.size(); ++i) {
          if (n_total_outs != sources[i].outputs.size()) {
            LOG_ERROR("Non-simple ringct transaction has varying ring size");
            return false;
          }
        }
      }

      uint64_t amount_in = 0, amount_out = 0;
      rct::ctkeyV inSk;
      inSk.reserve(sources.size());
      // mixRing indexing is done the other way round for simple
      rct::ctkeyM mixRing(use_simple_rct ? sources.size() : n_total_outs);
      rct::keyV destinations;
      std::vector<uint64_t> inamounts, outamounts;
      std::vector<unsigned int> index;
      for (size_t i = 0; i < sources.size(); ++i)
      {
        rct::ctkey ctkey;
        amount_in += sources[i].amount;
        inamounts.push_back(sources[i].amount);
        index.push_back(sources[i].real_output);
        // inSk: (secret key, mask)
        ctkey.dest = rct::sk2rct(in_contexts[i].in_ephemeral.sec);
        ctkey.mask = sources[i].mask;
        inSk.push_back(ctkey);
        memwipe(&ctkey, sizeof(rct::ctkey));
        // inPk: (public key, commitment)
        // will be done when filling in mixRing
      }
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        crypto::public_key output_public_key;
        get_output_public_key(tx.vout[i], output_public_key);
        destinations.push_back(rct::pk2rct(output_public_key));
        outamounts.push_back(tx.vout[i].amount);
        amount_out += tx.vout[i].amount;
      }

      if (use_simple_rct)
      {
        // mixRing indexing is done the other way round for simple
        for (size_t i = 0; i < sources.size(); ++i)
        {
          mixRing[i].resize(sources[i].outputs.size());
          for (size_t n = 0; n < sources[i].outputs.size(); ++n)
          {
            mixRing[i][n] = sources[i].outputs[n].second;
          }
        }
      }
      else
      {
        for (size_t i = 0; i < n_total_outs; ++i) // same index assumption
        {
          mixRing[i].resize(sources.size());
          for (size_t n = 0; n < sources.size(); ++n)
          {
            mixRing[i][n] = sources[n].outputs[i].second;
          }
        }
      }

      // fee
      if (!use_simple_rct && amount_in > amount_out)
        outamounts.push_back(amount_in - amount_out);

      // zero out all amounts to mask rct outputs, real amounts are now encrypted
      for (size_t i = 0; i < tx.vin.size(); ++i)
      {
        if (sources[i].rct)
          boost::get<txin_to_key>(tx.vin[i]).amount = 0;
      }
      for (size_t i = 0; i < tx.vout.size(); ++i)
        tx.vout[i].amount = 0;

      crypto::hash tx_prefix_hash;
      get_transaction_prefix_hash(tx, tx_prefix_hash, hwdev);
      rct::ctkeyV outSk;
      if (use_simple_rct)
        tx.rct_signatures = rct::genRctSimple(rct::hash2rct(tx_prefix_hash), inSk, destinations, inamounts, outamounts, amount_in - amount_out, mixRing, amount_keys, index, outSk, rct_config, hwdev);
      else
        tx.rct_signatures = rct::genRct(rct::hash2rct(tx_prefix_hash), inSk, destinations, outamounts, mixRing, amount_keys, sources[0].real_output, outSk, rct_config, hwdev); // same index assumption
      memwipe(inSk.data(), inSk.size() * sizeof(rct::ctkey));

      CHECK_AND_ASSERT_MES(tx.vout.size() == outSk.size(), false, "outSk size does not match vout");

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL);
    }

    tx.invalidate_hashes();

    return true;
  }
  //---------------------------------------------------------------
  bool construct_tx_and_get_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, crypto::secret_key &tx_key, std::vector<crypto::secret_key> &additional_tx_keys, bool rct, const rct::RCTConfig &rct_config, bool use_view_tags)
  {
    hw::device &hwdev = sender_account_keys.get_device();
    hwdev.open_tx(tx_key);
    try {
      // figure out if we need to make additional tx pubkeys
      size_t num_stdaddresses = 0;
      size_t num_subaddresses = 0;
      account_public_address single_dest_subaddress;
      classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);
      bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
      if (need_additional_txkeys)
      {
        additional_tx_keys.clear();
        for (size_t i = 0; i < destinations.size(); ++i)
        {
          additional_tx_keys.push_back(keypair::generate(sender_account_keys.get_device()).sec);
        }
      }

      bool shuffle_outs = true;
      bool r = construct_tx_with_tx_key(sender_account_keys, subaddresses, sources, destinations, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys, rct, rct_config, shuffle_outs, use_view_tags);
      hwdev.close_tx();
      return r;
    } catch(...) {
      hwdev.close_tx();
      throw;
    }
  }
  //---------------------------------------------------------------
  bool construct_tx(const account_keys& sender_account_keys, std::vector<tx_source_entry>& sources, const std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time)
  {
     std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
     subaddresses[sender_account_keys.m_account_address.m_spend_public_key] = {0,0};
     crypto::secret_key tx_key;
     std::vector<crypto::secret_key> additional_tx_keys;
     std::vector<tx_destination_entry> destinations_copy = destinations;
     return construct_tx_and_get_tx_key(sender_account_keys, subaddresses, sources, destinations_copy, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys, false, { rct::RangeProofBorromean, 0});
  }
  //---------------------------------------------------------------
  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    )
  {
    //genesis block
    bl = {};

    blobdata tx_bl;
    bool r = string_tools::parse_hexstr_to_binbuff(genesis_tx, tx_bl);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    r = parse_and_validate_tx_from_blob(tx_bl, bl.miner_tx);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    bl.major_version = CURRENT_BLOCK_MAJOR_VERSION;
    bl.minor_version = CURRENT_BLOCK_MINOR_VERSION;
    bl.timestamp = 0;
    bl.nonce = nonce;
    miner::find_nonce_for_given_block([](const cryptonote::block &b, uint64_t height, const crypto::hash *seed_hash, unsigned int threads, crypto::hash &hash){
      return cryptonote::get_block_longhash(NULL, b, hash, height, seed_hash, threads);
    }, bl, 1, 0, NULL);
    bl.invalidate_hashes();
    return true;
  }
  //---------------------------------------------------------------
  void get_altblock_longhash(const block& b, crypto::hash& res, const uint64_t main_height, const uint64_t height, const uint64_t seed_height, const crypto::hash& seed_hash)
  {
    blobdata bd = get_block_hashing_blob(b);
    rx_slow_hash(main_height, seed_height, seed_hash.data, bd.data(), bd.size(), res.data, 0, 1);
  }

  bool get_block_longhash(const Blockchain *pbc, const blobdata& bd, crypto::hash& res, const uint64_t height, const int major_version, const crypto::hash *seed_hash, const int miners)
  {
    // block 202612 bug workaround
    if (height == 202612)
    {
      static const std::string longhash_202612 = "84f64766475d51837ac9efbef1926486e58563c95a19fef4aec3254f03000000";
      epee::string_tools::hex_to_pod(longhash_202612, res);
      return true;
    }
    if (major_version >= RX_BLOCK_VERSION)
    {
      uint64_t seed_height, main_height;
      crypto::hash hash;
      if (pbc != NULL)
      {
        seed_height = rx_seedheight(height);
        hash = seed_hash ? *seed_hash : pbc->get_pending_block_id_by_height(seed_height);
        main_height = pbc->get_current_blockchain_height();
      } else
      {
        memset(&hash, 0, sizeof(hash));  // only happens when generating genesis block
        seed_height = 0;
        main_height = 0;
      }
      rx_slow_hash(main_height, seed_height, hash.data, bd.data(), bd.size(), res.data, seed_hash ? 0 : miners, !!seed_hash);
    } else {
      const int pow_variant = major_version >= 7 ? major_version - 6 : 0;
      crypto::cn_slow_hash(bd.data(), bd.size(), res, pow_variant, height);
    }
    return true;
  }

  bool get_block_longhash(const Blockchain *pbc, const block& b, crypto::hash& res, const uint64_t height, const crypto::hash *seed_hash, const int miners)
  {
    blobdata bd = get_block_hashing_blob(b);
	return get_block_longhash(pbc, bd, res, height, b.major_version, seed_hash, miners);
  }

  bool get_block_longhash(const Blockchain *pbc, const block& b, crypto::hash& res, const uint64_t height, const int miners)
  {
    return get_block_longhash(pbc, b, res, height, NULL, miners);
  }

  crypto::hash get_block_longhash(const Blockchain *pbc, const block& b, const uint64_t height, const int miners)
  {
    crypto::hash p = crypto::null_hash;
    get_block_longhash(pbc, b, p, height, miners);
    return p;
  }

  void get_block_longhash_reorg(const uint64_t split_height)
  {
    rx_reorg(split_height);
  }
}
