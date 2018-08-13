//
// Created by Orange on 2018/8/8.
//

#include <eosiolib/eosio.hpp>
#include <eosiolib/asset.hpp>
#include <eosio.token/eosio.token.hpp>
#include <math.h>

#define GAME_SYMBOL S(4, CGT)
#define TOKEN_CONTRACT N(eosio.token)
#define BURN_ACCOUNT N(blackholeeos)
#define FEE_ACCOUNT N(playeronefee)

typedef double real_type;

using namespace eosio;
using namespace std;

class playerone: public contract {
public:
    const real_type _A = real_type(0.90);
    const real_type _B = real_type(0.05);
    const real_type _L = real_type(300000);
    const real_type _D = real_type(75000);
    const real_type _INITIAL_PRICE = real_type(0.01);
    const real_type _MAX_SUPPLY_TIMES = 20;
    const int32_t _GAME_INIT_TIME = 1535124913;
    const int32_t _ACTION_COOL_DOWN = 5;                       // 操作冷却时间(s) 

    playerone(account_name self)
        : contract(self), 
          _game(_self, _self),
          users(_self, _self)
    {

        // Create a new game if not exists
        auto game_itr = _game.find(_self);
        if (game_itr == _game.end())
        {
            game_itr = _game.emplace(_self, [&](auto& g){
                g.gameid = _self;
                g.max_supply = asset(_L * _MAX_SUPPLY_TIMES * 10000ll, GAME_SYMBOL);
            });

            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(create),
                make_tuple(_self, game_itr->max_supply))
            .send();
        }

        auto user_itr = users.find(FEE_ACCOUNT);
        if(user_itr == users.end()){
            user_itr = users.emplace(_self, [&](auto& u){
                u.name = FEE_ACCOUNT;
                u.parent = FEE_ACCOUNT;
            });
        }
    };

    void transfer(account_name from, account_name to, asset quantity, string memo){
        if (from == _self || to != _self) {
            return;
        }
        eosio_assert(quantity.is_valid(), "invalid token transfer");
        eosio_assert(quantity.amount > 0, "quantity must be positive");

        if(quantity.symbol == CORE_SYMBOL){
            eosio_assert(quantity.symbol == CORE_SYMBOL, "unexpected asset symbol input");
            eosio_assert(now() > _GAME_INIT_TIME, "game will start at 1535124913");
            if(memo == "deposit"){
                deposit(from, quantity, memo);
            } else {
                buy(from, quantity, memo);
            }
        } else if(quantity.symbol == GAME_SYMBOL) {
            eosio_assert(quantity.symbol == GAME_SYMBOL, "unexpected asset symbol input");
            if(memo == "burn"){
                burn(from, quantity, memo);
            } else {
                sell(from, quantity, memo);
            }
        } else {
            eosio_assert(false, "do not send other funds to this contract");
        }
    };

    void buy(account_name account, asset quantity, string memo){
        require_auth(account);
        eosio_assert(quantity.symbol == CORE_SYMBOL, "unexpected asset symbol input");
        eosio_assert(quantity.amount >= 10000ll && quantity.amount <= 100 * 10000ll, "quantity must in range 1 - 100 EOS");

        asset exchange_unit = asset(10 * 10000ll, CORE_SYMBOL);
        int64_t times = (quantity / exchange_unit) + 1;

        asset deposited_eos = asset(0, CORE_SYMBOL);
        asset insured_eos = asset(0, CORE_SYMBOL);
        asset exchanged_eos = asset(0, CORE_SYMBOL);
        asset issued_eos = asset(0, CORE_SYMBOL);
        asset fee = quantity;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (first round up)
        
        asset quant_after_fee = quantity;
        quant_after_fee.amount -= fee.amount;

        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            auto parent = string_to_name(memo.c_str());
            auto parent_itr = users.find(parent);
            if(memo.size() <= 0 || memo.size()> 12 || parent == _self || account == parent || parent_itr == users.end() || account == FEE_ACCOUNT){
                parent = FEE_ACCOUNT;
            }
            user_itr = users.emplace(account, [&](auto& u ) {
                u.name = _self;
                u.parent = parent;
                u.last_action = now();
            });
        } else {
            eosio_assert(now() - user_itr->last_action >= _ACTION_COOL_DOWN, "action needs 5 seconds to cool down");
            users.modify(user_itr, 0, [&](auto& u ) {
                u.last_action = now();
            });
        }

        if (fee.amount > 0)
        {
            auto refer_fee = fee;
            refer_fee.amount = fee.amount / 2;
            fee.amount -= refer_fee.amount;

            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, user_itr->parent, fee, string("refer fee")))
            .send();

            auto parent_itr = users.find(user_itr->parent);

            if (refer_fee.amount > 0)
            {
                action(
                    permission_level{_self, N(active)},
                    TOKEN_CONTRACT, N(transfer),
                    make_tuple(_self, parent_itr->parent, refer_fee, string("refer fee")))
                .send();
            }
        }

        fee.amount = quant_after_fee.amount;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (second round up)
        asset action_total_fee = fee;

        quant_after_fee.amount -= fee.amount;

        asset remain_eos = quant_after_fee;
        asset transfer_token = asset(0, GAME_SYMBOL);
        asset issue_token = asset(0, GAME_SYMBOL);

        auto game_itr = _game.begin();
        eosio_assert(game_itr != _game.end(), "game is not inited");
        asset reserve_balance = game_itr->reserve;
        asset token_supply = game_itr->supply;
        asset token_balance = game_itr->balance;

        eosio_assert(token_supply >= token_balance, "shit happens");

        asset circulation = token_supply - token_balance;
        real_type crr;
        real_type token_price;
        for(int64_t i = 0; i < times; i++){
            if(remain_eos <= asset(0, CORE_SYMBOL)){
                break;
            }
            if(exchange_unit > remain_eos){
                exchange_unit = remain_eos;
            }
            if(circulation > asset(10000, CORE_SYMBOL) && token_balance > asset(0, CORE_SYMBOL)){
                crr = _crr(circulation);

                //TODO test the cast from real_type to uint64_t

                token_price = reserve_balance / (circulation * crr);

                //TODO test the cast from asset to uint64_t

                asset token_per_exchange = asset((exchange_unit / token_price).amount, GAME_SYMBOL);
                crr = _crr(circulation + token_per_exchange);

                //TODO test the cast from real_type to uint64_t

                token_price = (reserve_balance + exchange_unit) / (circulation * crr);
                token_per_exchange = asset((exchange_unit / token_price).amount, GAME_SYMBOL);
                if(token_balance >= token_per_exchange){
                    circulation += token_per_exchange;
                    token_balance -= token_per_exchange;
                    transfer_token += token_per_exchange;
                    deposited_eos += exchange_unit;
                    remain_eos -= exchange_unit;
                    exchanged_eos += exchange_unit;
                    reserve_balance += exchange_unit;
                } else {
                    token_per_exchange = token_balance;
                    crr = _crr(circulation + token_per_exchange);

                    //TODO test the cast from real_type to uint64_t

                    token_price = (reserve_balance + exchange_unit) / (circulation * crr);
                    circulation += token_per_exchange;
                    token_balance += token_per_exchange;
                    transfer_token += token_per_exchange;

                    //TODO test the cast from asset to uint64_t

                    asset to_deposit_eos = asset((token_price * token_per_exchange).amount, CORE_SYMBOL);
                    deposited_eos += to_deposit_eos;
                    remain_eos -= to_deposit_eos;
                    exchanged_eos += to_deposit_eos;
                    reserve_balance += to_deposit_eos;
                }
            } else {
                crr = _crr(circulation);
                asset to_issue_eos = exchange_unit * crr;

                //TODO test the cast from real_type to uint64_t

                asset token_per_issue = asset((to_issue_eos / _INITIAL_PRICE).amount, GAME_SYMBOL);
                circulation += token_per_issue;
                issue_token += token_per_issue;
                deposited_eos += to_issue_eos;
                insured_eos += exchange_unit - to_issue_eos;
                remain_eos -= exchange_unit;
                issued_eos += exchange_unit;
                reserve_balance += to_issue_eos;
            }
        }

        asset refund_eos = quantity - deposited_eos - insured_eos;

        eosio_assert(refund_eos >= asset(0, CORE_SYMBOL) && refund_eos <= quantity, "invalid refund token");
        eosio_assert(exchanged_eos + issued_eos == deposited_eos + insured_eos, "exchanged token not equal");
        eosio_assert(refund_eos == remain_eos, "refund value not equal");
        eosio_assert(quantity - remain_eos == deposited_eos + insured_eos, "refund token not equal");
        eosio_assert(transfer_token + issue_token >= asset(10000ll, GAME_SYMBOL) && transfer_token + issue_token <= asset(1000 * 10000ll, GAME_SYMBOL), "transfer and issue token must in range 1 - 1000");

        _game.modify(game_itr, 0, [&](auto& g) {
            g.reserve += deposited_eos;
            g.insure += insured_eos + action_total_fee;
            g.supply += issue_token;
            g.balance = token_balance;
            g.circulation = circulation;
            g.crr = crr;
        });

        if(refund_eos > asset(0, CORE_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, refund_eos, string("refund")))
            .send();
        }

        if(transfer_token > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, transfer_token, string("buy")))
            .send();
        }

        if(issue_token > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(issue),
                make_tuple(account, issue_token, string("issue")))
            .send();
        }

        auto eos_token = eosio::token(TOKEN_CONTRACT);
        asset real_eos_balance = eos_token.get_balance(_self, symbol_type(CORE_SYMBOL).name());
        asset real_token_supply = eos_token.get_supply(symbol_type(GAME_SYMBOL).name());
        asset real_token_balance = eos_token.get_balance(_self, symbol_type(GAME_SYMBOL).name());
        eosio_assert(real_eos_balance == game_itr->reserve + game_itr->insure, "eos balance leaks");
        eosio_assert(real_token_supply == game_itr->supply, "token supply leaks");
        eosio_assert(real_token_balance == game_itr->balance, "token balance leaks");
        eosio_assert(real_token_supply - real_token_balance == game_itr->circulation && game_itr->circulation >= asset(0, GAME_SYMBOL), "circulation leaks");

    }

    void sell(account_name account, asset quantity, string memo){
        require_auth(account);
        eosio_assert(quantity.symbol == GAME_SYMBOL, "unexpected asset symbol input");
        eosio_assert(quantity.amount >= 10000ll && quantity.amount <= 1000 * 10000ll, "quantity must in range 1 - 1000 CGT");
        asset exchange_unit = asset(100 * 10000ll, CORE_SYMBOL);
        asset remain_asset = quantity;
        int64_t times = (quantity / exchange_unit) + 1;

        asset transfer_eos = asset(0, CORE_SYMBOL);

        auto game_itr = _game.begin();
        eosio_assert(game_itr != _game.end(), "game is not inited");

        asset reserve_balance = game_itr->reserve;
        asset token_supply = game_itr->supply;
        asset token_balance = game_itr->balance;

        eosio_assert(token_supply >= token_balance, "shit happens");

        asset circulation = token_supply - token_balance;
        real_type crr;
        real_type token_price;

        for(int64_t i = 0; i < times; i++){
            if(remain_asset <= asset(0, GAME_SYMBOL)){
                break;
            }
            if(exchange_unit > remain_asset){
                exchange_unit = remain_asset;
            }
            crr = _crr(circulation);

            //TODO

            token_price = reserve_balance / (circulation * crr);

            //TODO

            asset eos_per_exchange = asset((exchange_unit * token_price).amount, CORE_SYMBOL);
            reserve_balance -= eos_per_exchange;
            if(reserve_balance < asset(0, CORE_SYMBOL)){
                eosio_assert(false, "insufficient reserve eos");
            }
            token_price = reserve_balance / (circulation * crr);

            //TODO

            eos_per_exchange = exchange_unit * token_price;
            transfer_eos += eos_per_exchange;
            circulation -= exchange_unit;
            remain_asset -= exchange_unit;
            token_balance += exchange_unit;
        }

        eosio_assert(transfer_eos <= asset(100 * 10000ll, CORE_SYMBOL) && transfer_eos >= asset(10000ll, CORE_SYMBOL), "sell in range 1 - 100 eos");
        eosio_assert(remain_asset >= asset(0, GAME_SYMBOL) && quantity >= remain_asset, "remain asset is invalid");
        asset _token_balance = game_itr->balance;
        eosio_assert(quantity - remain_asset == token_balance - _token_balance, "exchange asset is not equal");
        eosio_assert(game_itr->reserve >= transfer_eos, "insufficient reserve eos");

        asset fee = transfer_eos;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (first round up)

        asset quant_after_fee = transfer_eos;
        quant_after_fee.amount -= fee.amount;

        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            auto parent = string_to_name(memo.c_str());
            auto parent_itr = users.find(parent);
            if(memo.size() <= 0 || memo.size()> 12 || parent == _self || account == parent || parent_itr == users.end() || account == FEE_ACCOUNT){
                parent = FEE_ACCOUNT;
            }
            user_itr = users.emplace(account, [&](auto& u ) {
                u.name = _self;
                u.parent = parent;
                u.last_action = now();
            });
        } else {
            eosio_assert(now() - user_itr->last_action >= _ACTION_COOL_DOWN, "action needs 5 seconds to cool down");
            users.modify(user_itr, 0, [&](auto& u ) {
                u.last_action = now();
            });
        }

        if (fee.amount > 0)
        {
            auto refer_fee = fee;
            refer_fee.amount = fee.amount / 2;
            fee.amount -= refer_fee.amount;

            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, user_itr->parent, fee, string("refer fee")))
            .send();

            auto parent_itr = users.find(user_itr->parent);

            if (refer_fee.amount > 0)
            {
                action(
                    permission_level{_self, N(active)},
                    TOKEN_CONTRACT, N(transfer),
                    make_tuple(_self, parent_itr->parent, refer_fee, string("refer fee")))
                .send();
            }
        }

        fee.amount = quant_after_fee.amount;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (second round up)
        asset action_total_fee = fee;

        quant_after_fee.amount -= fee.amount;

        _game.modify(game_itr, 0, [&](auto& g) {
            g.reserve -= transfer_eos;
            g.insure += action_total_fee;
            g.balance = token_balance;
            g.circulation = circulation;
            g.crr = crr;
        });

        if(remain_asset > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, remain_asset, string("refund")))
            .send();
        }

        if(transfer_eos > asset(0, CORE_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, quant_after_fee, string("sell")))
            .send();
        }

        auto eos_token = eosio::token(TOKEN_CONTRACT);
        asset real_eos_balance = eos_token.get_balance(_self, symbol_type(CORE_SYMBOL).name());
        asset real_token_supply = eos_token.get_supply(symbol_type(GAME_SYMBOL).name());
        asset real_token_balance = eos_token.get_balance(_self, symbol_type(GAME_SYMBOL).name());
        eosio_assert(real_eos_balance == game_itr->reserve + game_itr->insure, "eos balance leaks");
        eosio_assert(real_token_supply == game_itr->supply, "token supply leaks");
        eosio_assert(real_token_balance == game_itr->balance, "token balance leaks");
        eosio_assert(real_token_supply - real_token_balance == game_itr->circulation && game_itr->circulation >= asset(0, GAME_SYMBOL), "circulation leaks");
    }

    void burn(account_name account, asset quantity, string memo){
        require_auth(account);
        eosio_assert(quantity.symbol == GAME_SYMBOL, "unexpected asset symbol input");
        eosio_assert(quantity.amount >= 10000ll && quantity.amount <= 1000 * 10000ll, "quantity must in range 1 - 1000 CGT");
        
        auto game_itr = _game.begin();
        eosio_assert(game_itr != _game.end(), "game is not inited");

        asset insure_balance = game_itr->reserve;
        asset token_supply = game_itr->supply;
        asset token_balance = game_itr->balance;

        eosio_assert(token_supply >= token_balance, "shit happens");

        asset circulation = token_supply - token_balance;

        //TODO

        real_type token_price = insure_balance / circulation;

        //TODO

        asset transfer_eos = asset((token_price * quantity).amount, CORE_SYMBOL);

        eosio_assert(transfer_eos <= asset(100 * 10000ll, CORE_SYMBOL) && transfer_eos >= asset(10000ll, CORE_SYMBOL), "burn in range 1 - 100 eos");
        eosio_assert(insure_balance >= transfer_eos, "insufficient insure eos");

        _game.modify(game_itr, 0, [&](auto& g) {
            g.insure -= transfer_eos;
            g.supply -= quantity;
            g.circulation -= quantity;
            g.burn += quantity;
        });

        asset fee = transfer_eos;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (round up)
        asset action_total_fee = fee;

        asset quant_after_fee = transfer_eos;
        quant_after_fee.amount -= fee.amount;

        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            auto parent = string_to_name(memo.c_str());
            auto parent_itr = users.find(parent);
            if(memo.size() <= 0 || memo.size()> 12 || parent == _self || account == parent || parent_itr == users.end() || account == FEE_ACCOUNT){
                parent = FEE_ACCOUNT;
            }
            user_itr = users.emplace(account, [&](auto& u ) {
                u.name = _self;
                u.parent = parent;
                u.last_action = now();
            });
        } else {
            eosio_assert(now() - user_itr->last_action >= _ACTION_COOL_DOWN, "action needs 5 seconds to cool down");
            users.modify(user_itr, 0, [&](auto& u ) {
                u.last_action = now();
            });
        }

        if (fee.amount > 0)
        {
            auto refer_fee = fee;
            refer_fee.amount = fee.amount / 2;
            fee.amount -= refer_fee.amount;

            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, user_itr->parent, fee, string("refer fee")))
            .send();

            auto parent_itr = users.find(user_itr->parent);

            if (refer_fee.amount > 0)
            {
                action(
                    permission_level{_self, N(active)},
                    TOKEN_CONTRACT, N(transfer),
                    make_tuple(_self, parent_itr->parent, refer_fee, string("refer fee")))
                .send();
            }
        }

        if(quantity > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, BURN_ACCOUNT, quantity, string("burn token")))
            .send();
        }

        if(transfer_eos > asset(0, CORE_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, transfer_eos, string("burn")))
            .send();
        }

        auto eos_token = eosio::token(TOKEN_CONTRACT);
        asset real_eos_balance = eos_token.get_balance(_self, symbol_type(CORE_SYMBOL).name());
        asset real_token_supply = eos_token.get_supply(symbol_type(GAME_SYMBOL).name());
        asset real_token_balance = eos_token.get_balance(_self, symbol_type(GAME_SYMBOL).name());
        eosio_assert(real_eos_balance == game_itr->reserve + game_itr->insure, "eos balance leaks");
        eosio_assert(real_token_supply == game_itr->supply, "token supply leaks");
        eosio_assert(real_token_balance == game_itr->balance, "token balance leaks");
        eosio_assert(real_token_supply - real_token_balance == game_itr->circulation && game_itr->circulation >= asset(0, GAME_SYMBOL), "circulation leaks");

    }

    void deposit(account_name account, asset quantity, string memo){
        eosio_assert(quantity.symbol == CORE_SYMBOL, "unexpected asset symbol input");
        eosio_assert(quantity.amount >= 0, "quantity must be positive");

        auto user_itr = users.find(account);
        if(user_itr == users.end() && quantity.amount >= 10000ll){
            auto parent = string_to_name(memo.c_str());
            auto parent_itr = users.find(parent);
            if(memo.size() <= 0 || memo.size()> 12 || parent == _self || account == parent || parent_itr == users.end() || account == FEE_ACCOUNT){
                parent = FEE_ACCOUNT;
            }
            user_itr = users.emplace(account, [&](auto& u ) {
                u.name = _self;
                u.parent = parent;
                u.last_action = now();
            });
        }
        
        auto game_itr = _game.begin();
        eosio_assert(game_itr != _game.end(), "game is not inited");

        _game.modify(game_itr, 0, [&](auto& g) {
            g.insure += quantity;
        });

        auto eos_token = eosio::token(TOKEN_CONTRACT);
        asset real_eos_balance = eos_token.get_balance(_self, symbol_type(CORE_SYMBOL).name());
        eosio_assert(real_eos_balance == game_itr->reserve + game_itr->insure, "eos balance leaks");
    }

    private:
    real_type _crr(asset circulation) {
        eosio_assert(circulation.amount >=0, "shit happens");
        real_type _X(circulation.amount / 10000ll);
        real_type ONE(1.0);
        real_type E(2.71828182845904);
        real_type R = ONE / (ONE + pow(E, (_X - _L) / _D)) * _A + _B;
        eosio_assert(R >= _B && R <= _B + _A, "shit happens");
        return R;
    }

    // @abi table game i64
    struct game{
        account_name gameid;
        asset reserve = asset(0, CORE_SYMBOL);
        asset insure = asset(0, CORE_SYMBOL);
        asset max_supply = asset(0, CORE_SYMBOL);
        asset supply = asset(0, GAME_SYMBOL);
        asset balance = asset(0, GAME_SYMBOL);
        asset circulation = asset(0, GAME_SYMBOL);
        asset burn = asset(0, GAME_SYMBOL);
        real_type crr = real_type(0.0);
        int64_t start_time = current_time();

        uint64_t primary_key() const { return gameid; }
        EOSLIB_SERIALIZE(game, (gameid)(reserve)(insure)(max_supply)(supply)(balance)(circulation)(burn)(crr)(start_time))
    };
    typedef eosio::multi_index<N(game), game> game_index;
    game_index _game;

    // @abi table users i64
    struct user{
        account_name name;
        account_name parent;
        int32_t last_action = now();
        uint64_t primary_key() const { return name; }
        EOSLIB_SERIALIZE(user, (name)(parent)(last_action))
    };
    typedef eosio::multi_index<N(users), user> user_index;
    user_index users;

};

#define EOSIO_ABI_EX( TYPE, MEMBERS ) \
extern "C" { \
    void apply( uint64_t receiver, uint64_t code, uint64_t action ) { \
        auto self = receiver; \
        if( action == N(onerror)) { \
            /* onerror is only valid if it is for the "eosio" code account and authorized by "eosio"'s "active permission */ \
            eosio_assert(code == N(eosio), "onerror action's are only valid from the \"eosio\" system account"); \
        } \
        if((code == TOKEN_CONTRACT && action == N(transfer))) { \
            TYPE thiscontract( self ); \
            switch( action ) { \
                EOSIO_API( TYPE, MEMBERS ) \
            } \
         /* does not allow destructor of thiscontract to run: eosio_exit(0); */ \
        } \
    } \
} \

EOSIO_ABI_EX(playerone, (transfer))