/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once

#include <graphene/chain/types.hpp>
#include <graphene/db/generic_index.hpp>
#include <graphene/protocol/asset.hpp>

#include <boost/multi_index/composite_key.hpp>

namespace graphene { namespace chain {

using namespace graphene::db;

/**
 *  @brief an offer to sell an amount of an asset at a specified exchange rate by a certain time
 *  @ingroup object
 *  @ingroup protocol
 *  @ingroup market
 *
 *  The objects are indexed by @ref expiration and are automatically deleted on the first block after expiration.
 */
class limit_order_object : public abstract_object<limit_order_object>
{
   public:
      static constexpr uint8_t space_id = protocol_ids;
      static constexpr uint8_t type_id  = limit_order_object_type;

      time_point_sec   expiration;
      account_id_type  seller;
      share_type       for_sale; ///< asset id is sell_price.base.asset_id
      price            sell_price;
      share_type       deferred_fee; ///< fee converted to CORE
      asset            deferred_paid_fee; ///< originally paid fee
      bool             is_settled_debt = false; ///< Whether this order is an individual settlement fund

      pair<asset_id_type,asset_id_type> get_market()const
      {
         auto tmp = std::make_pair( sell_price.base.asset_id, sell_price.quote.asset_id );
         if( tmp.first > tmp.second ) std::swap( tmp.first, tmp.second );
         return tmp;
      }

      asset amount_for_sale()const   { return asset( for_sale, sell_price.base.asset_id ); }
      asset amount_to_receive()const { return amount_for_sale() * sell_price; }
      asset_id_type sell_asset_id()const    { return sell_price.base.asset_id;  }
      asset_id_type receive_asset_id()const { return sell_price.quote.asset_id; }
};

struct by_price;
struct by_expiration;
struct by_account;
struct by_account_price;
struct by_is_settled_debt;
typedef multi_index_container<
   limit_order_object,
   indexed_by<
      ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
      ordered_unique< tag<by_expiration>,
         composite_key< limit_order_object,
            member< limit_order_object, time_point_sec, &limit_order_object::expiration>,
            member< object, object_id_type, &object::id>
         >
      >,
      ordered_unique< tag<by_price>,
         composite_key< limit_order_object,
            member< limit_order_object, price, &limit_order_object::sell_price>,
            member< object, object_id_type, &object::id>
         >,
         composite_key_compare< std::greater<price>, std::less<object_id_type> >
      >,
      ordered_unique< tag<by_is_settled_debt>,
         composite_key< limit_order_object,
            member< limit_order_object, bool, &limit_order_object::is_settled_debt >,
            const_mem_fun< limit_order_object, asset_id_type, &limit_order_object::receive_asset_id >,
            member< object, object_id_type, &object::id>
         >
      >,
      // index used by APIs
      ordered_unique< tag<by_account>,
         composite_key< limit_order_object,
            member<limit_order_object, account_id_type, &limit_order_object::seller>,
            member<object, object_id_type, &object::id>
         >
      >,
      // index used by APIs
      ordered_unique< tag<by_account_price>,
         composite_key< limit_order_object,
            member<limit_order_object, account_id_type, &limit_order_object::seller>,
            member<limit_order_object, price, &limit_order_object::sell_price>,
            member<object, object_id_type, &object::id>
         >,
         composite_key_compare<std::less<account_id_type>, std::greater<price>, std::less<object_id_type>>
      >
   >
> limit_order_multi_index_type;

typedef generic_index<limit_order_object, limit_order_multi_index_type> limit_order_index;

/**
 * @class call_order_object
 * @brief tracks debt and call price information
 *
 * There should only be one call_order_object per asset pair per account and
 * they will all have the same call price.
 */
class call_order_object : public abstract_object<call_order_object>
{
   public:
      static constexpr uint8_t space_id = protocol_ids;
      static constexpr uint8_t type_id  = call_order_object_type;

      asset get_collateral()const { return asset( collateral, call_price.base.asset_id ); }
      asset get_debt()const { return asset( debt, debt_type() ); }
      asset amount_to_receive()const { return get_debt(); }
      asset_id_type debt_type()const { return call_price.quote.asset_id; }
      asset_id_type collateral_type()const { return call_price.base.asset_id; }
      price collateralization()const { return get_collateral() / get_debt(); }

      account_id_type  borrower;
      share_type       collateral;  ///< call_price.base.asset_id, access via get_collateral
      share_type       debt;        ///< call_price.quote.asset_id, access via get_debt
      price            call_price;  ///< Collateral / Debt

      optional<uint16_t> target_collateral_ratio; ///< maximum CR to maintain when selling collateral on margin call

      pair<asset_id_type,asset_id_type> get_market()const
      {
         auto tmp = std::make_pair( call_price.base.asset_id, call_price.quote.asset_id );
         if( tmp.first > tmp.second ) std::swap( tmp.first, tmp.second );
         return tmp;
      }

      /**
       *  Calculate maximum quantity of debt to cover to satisfy @ref target_collateral_ratio.
       *
       *  @param match_price the matching price if this call order is margin called
       *  @param feed_price median settlement price of debt asset
       *  @param maintenance_collateral_ratio median maintenance collateral ratio of debt asset
       *  @param maintenance_collateralization maintenance collateralization of debt asset,
       *                                       should only be valid after core-1270 hard fork
       *  @return maximum amount of debt that can be called
       */
      share_type get_max_debt_to_cover( price match_price,
                                        price feed_price,
                                        const uint16_t maintenance_collateral_ratio,
                                        const optional<price>& maintenance_collateralization = optional<price>()
                                      )const;
};

/**
 *  @brief tracks bitassets scheduled for force settlement at some point in the future.
 *
 *  On the @ref settlement_date the @ref balance will be converted to the collateral asset
 *  and paid to @ref owner and then this object will be deleted.
 */
class force_settlement_object : public abstract_object<force_settlement_object>
{
   public:
      static constexpr uint8_t space_id = protocol_ids;
      static constexpr uint8_t type_id  = force_settlement_object_type;

      account_id_type   owner;
      asset             balance;
      time_point_sec    settlement_date;

      asset_id_type settlement_asset_id()const
      { return balance.asset_id; }
};

/**
 * @class collateral_bid_object
 * @brief bids of collateral for debt after a black swan
 *
 * There should only be one collateral_bid_object per asset per account, and
 * only for smartcoin assets that have a global settlement_price.
 */
class collateral_bid_object : public abstract_object<collateral_bid_object>
{
   public:
      static constexpr uint8_t space_id = implementation_ids;
      static constexpr uint8_t type_id  = impl_collateral_bid_object_type;

      asset get_additional_collateral()const { return inv_swan_price.base; }
      asset get_debt_covered()const { return inv_swan_price.quote; }
      asset_id_type debt_type()const { return inv_swan_price.quote.asset_id; }

      account_id_type  bidder;
      price            inv_swan_price;  // Collateral / Debt
};

struct by_collateral;
struct by_account;
struct by_price;
typedef multi_index_container<
   call_order_object,
   indexed_by<
      ordered_unique< tag<by_id>,
         member< object, object_id_type, &object::id > >,
      ordered_unique< tag<by_price>,
         composite_key< call_order_object,
            member< call_order_object, price, &call_order_object::call_price>,
            member< object, object_id_type, &object::id>
         >,
         composite_key_compare< std::less<price>, std::less<object_id_type> >
      >,
      ordered_unique< tag<by_account>,
         composite_key< call_order_object,
            member< call_order_object, account_id_type, &call_order_object::borrower >,
            const_mem_fun< call_order_object, asset_id_type, &call_order_object::debt_type>
         >
      >,
      ordered_unique< tag<by_collateral>,
         composite_key< call_order_object,
            const_mem_fun< call_order_object, price, &call_order_object::collateralization >,
            member< object, object_id_type, &object::id >
         >
      >
   >
> call_order_multi_index_type;

struct by_expiration;
typedef multi_index_container<
   force_settlement_object,
   indexed_by<
      ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
      ordered_unique< tag<by_account>,
         composite_key< force_settlement_object,
            member<force_settlement_object, account_id_type, &force_settlement_object::owner>,
            member< object, object_id_type, &object::id >
         >
      >,
      ordered_unique< tag<by_expiration>,
         composite_key< force_settlement_object,
            const_mem_fun<force_settlement_object, asset_id_type, &force_settlement_object::settlement_asset_id>,
            member<force_settlement_object, time_point_sec, &force_settlement_object::settlement_date>,
            member< object, object_id_type, &object::id >
         >
      >
   >
> force_settlement_object_multi_index_type;

typedef multi_index_container<
   collateral_bid_object,
   indexed_by<
      ordered_unique< tag<by_id>,
         member< object, object_id_type, &object::id > >,
      ordered_unique< tag<by_account>,
         composite_key< collateral_bid_object,
            const_mem_fun< collateral_bid_object, asset_id_type, &collateral_bid_object::debt_type>,
            member< collateral_bid_object, account_id_type, &collateral_bid_object::bidder>
         >
      >,
      ordered_unique< tag<by_price>,
         composite_key< collateral_bid_object,
            const_mem_fun< collateral_bid_object, asset_id_type, &collateral_bid_object::debt_type>,
            member< collateral_bid_object, price, &collateral_bid_object::inv_swan_price >,
            member< object, object_id_type, &object::id >
         >,
         composite_key_compare< std::less<asset_id_type>, std::greater<price>, std::less<object_id_type> >
      >
   >
> collateral_bid_object_multi_index_type;

typedef generic_index<call_order_object, call_order_multi_index_type>                      call_order_index;
typedef generic_index<force_settlement_object, force_settlement_object_multi_index_type>   force_settlement_index;
typedef generic_index<collateral_bid_object, collateral_bid_object_multi_index_type>       collateral_bid_index;

} } // graphene::chain

MAP_OBJECT_ID_TO_TYPE(graphene::chain::limit_order_object)
MAP_OBJECT_ID_TO_TYPE(graphene::chain::call_order_object)
MAP_OBJECT_ID_TO_TYPE(graphene::chain::force_settlement_object)
MAP_OBJECT_ID_TO_TYPE(graphene::chain::collateral_bid_object)

FC_REFLECT_TYPENAME( graphene::chain::limit_order_object )
FC_REFLECT_TYPENAME( graphene::chain::call_order_object )
FC_REFLECT_TYPENAME( graphene::chain::force_settlement_object )
FC_REFLECT_TYPENAME( graphene::chain::collateral_bid_object )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::limit_order_object )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::call_order_object )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::force_settlement_object )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::collateral_bid_object )
