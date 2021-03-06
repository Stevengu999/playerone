//
// Created by Orange on 2018/8/8.
//

#include <eosiolib/eosio.hpp>
#include <eosiolib/asset.hpp>
#include <math.h>

#define GAME_SYMBOL S(4, CGT)
#define TOKEN_CONTRACT N(eosio.token)
#define GAME_TOKEN_CONTRACT N(playeroneiss)
#define BURN_ACCOUNT N(blackholeeos)
#define FEE_ACCOUNT N(playeronefee)
#define CPUBANK_ACCOUNT N(eosiocpubank)

typedef double real_type;

using namespace eosio;
using namespace std;

class playerone: public contract {
public:
    const int64_t _B = 5ll;
    const int64_t _A = 100ll - _B * 2;
    const int64_t _L = 1000000ll;
    const int64_t _D = _L / 4;
    const int64_t _INITIAL_PRICE = 100ll;
    const int64_t _MAX_SUPPLY_TIMES = 10ll;
    //TODO set the time to future game init time
    const int64_t _GAME_INIT_TIME = 1534582387ll;
    const int64_t _GAME_PRESALE_TIME = _GAME_INIT_TIME + 60 * 60ll;
    //TODO 1 second to cool down
    const int64_t _ACTION_COOL_DOWN = 0ll;
    const int64_t _UNIT = 10000ll;
    const int64_t _REFER_PRICE = _UNIT;

    playerone(account_name self)
        : contract(self), 
        _game(_self, _self),
        users(_self, _self),
        refers(_self, _self)
    {
        // Create a new game if not exists
        auto game_itr = _game.begin();
        if (game_itr == _game.end())
        {
            game_itr = _game.emplace(_self, [&](auto& g){
                g.gameid = _self;
                g.max_supply = asset(_L * _MAX_SUPPLY_TIMES * _UNIT, GAME_SYMBOL);
                g.start_time = _GAME_INIT_TIME;
            });
        }

        auto user_itr = users.find(FEE_ACCOUNT);
        if(user_itr == users.end()){
            user_itr = users.emplace(_self, [&](auto& u){
                u.name = FEE_ACCOUNT;
                u.parent = FEE_ACCOUNT;
                u.refer = 100;
                u.discount = 1;
            });
        }

        auto refer_itr = refers.find(FEE_ACCOUNT);
        if(refer_itr == refers.end() && user_itr->refer > 0){
            refer_itr = refers.emplace(_self, [&](auto& r){
                r.name = FEE_ACCOUNT;
            });
        }
    };

    void eosio_token_transfer(account_name from, account_name to, asset quantity, string memo){
        require_auth(from);
        if (from == _self || to != _self) {
            return;
        }
        eosio_assert(quantity.is_valid(), "invalid token transfer");
        eosio_assert(quantity.symbol == CORE_SYMBOL, "unexpected asset symbol input");

        transfer(from, to, quantity, memo);
    };

    void game_token_transfer(account_name from, account_name to, asset quantity, string memo){
        require_auth(from);
        if (from == _self || to != _self) {
            return;
        }
        eosio_assert(quantity.is_valid(), "invalid token transfer");
        eosio_assert(quantity.symbol == GAME_SYMBOL, "unexpected asset symbol input");

        transfer(from, to, quantity, memo);
    };

    void transfer(account_name from, account_name to, asset quantity, string memo){
        require_auth(from);
        if (from == _self || to != _self) {
            return;
        }
        eosio_assert(quantity.is_valid(), "invalid token transfer");

        if(quantity.symbol == CORE_SYMBOL){
            if(quantity.amount == 1ll){
                claim_fee(from);
            } else if(quantity.amount == 2ll){
                unstake(from);
            } else if(memo == "deposit"){
                deposit(from, quantity, memo);
            } else if(memo == "1d" || memo == "4d" || memo == "7d") {
                eosio_assert(quantity.amount >= 50ll && quantity.amount <= 10000ll, "lease cpu in range 0.005 - 1 EOS");
                action(
                    permission_level{_self, N(active)},
                    TOKEN_CONTRACT, N(transfer),
                    make_tuple(_self, CPUBANK_ACCOUNT, quantity, memo))
                .send();
            } else {
                eosio_assert( now() >= _GAME_INIT_TIME, "can not buy at this moment");
                if( now() < _GAME_PRESALE_TIME ){
                    auto user_itr = users.find(from);
                    if(user_itr == users.end() || quantity.amount > user_itr->refer * 20000ll){
                        eosio_assert( quantity.amount >= 10000ll && quantity.amount <= 10 * 10000ll, "insufficient quota in pre-sale, deposit to get two times quota or buy in range 1 - 10 EOS");
                    } else if(quantity.amount > 10 * 10000ll) {
                        users.modify(user_itr, 0, [&](auto& u){
                            u.refer -= quantity.amount / 20000ll;
                        });
                    }
                }
                buy(from, quantity, memo);
            }
        } else if(quantity.symbol == GAME_SYMBOL) {
            eosio_assert( now() >= _GAME_PRESALE_TIME, "can not burn or sell in presale");
            if(memo == "burn"){
                burn(from, quantity, memo);
            } else if(memo == "stake") {
                stake(from, quantity, memo);
            } else {
                sell(from, quantity, memo);
            }
        } else {
            eosio_assert(false, "do not send other funds to this contract");
        }
    };

    void buy(account_name account, asset quantity, string memo){
        eosio_assert(quantity.amount >= _UNIT && quantity.amount <= 100 * _UNIT, "quantity must in range 1 - 100 EOS");

        asset exchange_unit = asset(10 * _UNIT, CORE_SYMBOL);
        int64_t times = (quantity / exchange_unit) + 1;
        asset deposited_eos = asset(0, CORE_SYMBOL);
        asset insured_eos = asset(0, CORE_SYMBOL);
        asset exchanged_eos = asset(0, CORE_SYMBOL);
        asset issued_eos = asset(0, CORE_SYMBOL);

        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            new_user(account, memo);
            user_itr = users.find(account);
        } else {
            uint64_t now_action = now();
            eosio_assert( now_action >= user_itr->last_action + _ACTION_COOL_DOWN, "action needs to cool down");
            if( now_action < _GAME_PRESALE_TIME){
                now_action += 225ll / (now_action - user_itr->last_action + 1);
            }
            users.modify(user_itr, 0, [&](auto& u) {
                u.last_action = now_action;
            });
        }    

        asset fee = quantity;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (first round up)
        
        asset quant_after_fee = quantity;
        quant_after_fee -= fee;

        collect_fee(account, fee);

        fee.amount = quant_after_fee.amount;
        if(user_itr->discount == 0){
            fee.amount = (fee.amount + 49) / 50; /// 2% fee (second round up)
        } else {
            fee.amount = (fee.amount + 99) / 100; /// 1% fee (discount half of the fee with a refer)
        }
        
        asset action_total_fee = fee;
        quant_after_fee -= fee;

        asset remain_eos = quant_after_fee;
        asset transfer_token = asset(0, GAME_SYMBOL);
        asset issue_token = asset(0, GAME_SYMBOL);
        auto game_itr = _game.begin();
        asset reserve_balance = game_itr->reserve;
        asset token_supply = game_itr->supply;
        asset token_balance = game_itr->balance;
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
            if(circulation > asset(100000 * _UNIT, GAME_SYMBOL) && token_balance > asset(0, GAME_SYMBOL)){
                crr = _crr(circulation);
                token_price = real_type(reserve_balance.amount) / (real_type(circulation.amount) * crr);
                asset token_per_exchange = asset(real_type(exchange_unit.amount) / token_price, GAME_SYMBOL);
                crr = _crr(circulation + token_per_exchange);
                token_price = real_type((reserve_balance + exchange_unit).amount) / (real_type(circulation.amount) * crr);
                token_per_exchange = asset(real_type(exchange_unit.amount) / token_price, GAME_SYMBOL);
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
                    token_price = real_type((reserve_balance + exchange_unit).amount) / (real_type(circulation.amount) * crr);
                    circulation += token_per_exchange;
                    token_balance -= token_per_exchange;
                    transfer_token += token_per_exchange;
                    asset to_deposit_eos = asset(token_price * real_type(token_per_exchange.amount), CORE_SYMBOL);
                    deposited_eos += to_deposit_eos;
                    remain_eos -= to_deposit_eos;
                    exchanged_eos += to_deposit_eos;
                    reserve_balance += to_deposit_eos;
                }
                eosio_assert(token_price >= real_type(0.0), "invalid token price");
            } else {
                crr = _crr(circulation);
                asset to_issue_eos = asset(real_type(exchange_unit.amount) * crr, exchange_unit.symbol);
                real_type INITIAL_PRICE(_INITIAL_PRICE);
                real_type UNIT(10000.0);
                INITIAL_PRICE = INITIAL_PRICE / UNIT;
                asset token_per_issue = asset(real_type(to_issue_eos.amount) / INITIAL_PRICE, GAME_SYMBOL);
                circulation += token_per_issue;
                issue_token += token_per_issue;
                deposited_eos += to_issue_eos;
                insured_eos += exchange_unit - to_issue_eos;
                remain_eos -= exchange_unit;
                issued_eos += exchange_unit;
                reserve_balance += to_issue_eos;
            }
        }

        asset refund_eos = quant_after_fee - deposited_eos - insured_eos;

        eosio_assert(transfer_token <= game_itr->balance, "insufficient token balance");
        eosio_assert(refund_eos == remain_eos && refund_eos >= asset(0, CORE_SYMBOL) && refund_eos <= quant_after_fee, "invalid eos refund");
        eosio_assert(deposited_eos >= asset(0, CORE_SYMBOL) && insured_eos >= asset(0, CORE_SYMBOL), "eos deposit or insure must be positive");
        eosio_assert(transfer_token >= asset(0, GAME_SYMBOL) && issue_token >= asset(0, GAME_SYMBOL), "transfer and issue token should not be negetive");
        eosio_assert(exchanged_eos + issued_eos == deposited_eos + insured_eos && quant_after_fee - remain_eos == deposited_eos + insured_eos, "eos not equal");
        eosio_assert(transfer_token + issue_token >= asset(_UNIT, GAME_SYMBOL) && transfer_token + issue_token <= asset(10000 * _UNIT, GAME_SYMBOL), "transfer and issue token must in range 1 - 10000");

        _game.modify(game_itr, 0, [&](auto& g) {
            g.reserve += deposited_eos;
            g.insure += insured_eos + fee / 2;
            g.reward += action_total_fee - fee / 2;
            g.supply += issue_token;
            g.balance = token_balance;
            g.circulation = circulation;
        });

        if(refund_eos > asset(0, CORE_SYMBOL)){
            users.modify(user_itr, 0, [&](auto& u){
                u.reward += refund_eos;
            });
        }

        if(transfer_token > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                GAME_TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, transfer_token, string("buy")))
            .send();
        }

        if(issue_token > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                GAME_TOKEN_CONTRACT, N(issue),
                make_tuple(account, issue_token, string("issue")))
            .send();
        }
    }

    void sell(account_name account, asset quantity, string memo){
        eosio_assert(quantity.amount >= _UNIT && quantity.amount <= 5000 * _UNIT, "quantity must in range 1 - 5000 CGT");
        asset exchange_unit = asset(1000 * _UNIT, GAME_SYMBOL);
        asset remain_asset = quantity;
        int64_t times = (quantity / exchange_unit) + 1;
        asset transfer_eos = asset(0, CORE_SYMBOL);
        auto game_itr = _game.begin();
        asset reserve_balance = game_itr->reserve;
        asset token_supply = game_itr->supply;
        asset token_balance = game_itr->balance;
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
            token_price = real_type(reserve_balance.amount) / (real_type(circulation.amount) * crr);
            asset eos_per_exchange = asset(real_type(exchange_unit.amount) * token_price, CORE_SYMBOL);
            reserve_balance -= eos_per_exchange;
            if(reserve_balance < asset(0, CORE_SYMBOL)){
                eosio_assert(false, "insufficient reserve eos");
            }
            token_price = reserve_balance.amount / (circulation.amount * crr);
            eos_per_exchange = asset(real_type(exchange_unit.amount) * token_price, CORE_SYMBOL);
            transfer_eos += eos_per_exchange;
            circulation -= exchange_unit;
            remain_asset -= exchange_unit;
            token_balance += exchange_unit;

            eosio_assert(token_price >= real_type(0.0), "invalid token price");
        }

        eosio_assert(transfer_eos <= asset(100 * _UNIT, CORE_SYMBOL) && transfer_eos >= asset(_UNIT, CORE_SYMBOL), "sell in range 1 - 100 eos");
        eosio_assert(remain_asset >= asset(0, GAME_SYMBOL) && quantity >= remain_asset, "remain asset is invalid");
        eosio_assert(quantity - remain_asset == token_balance - game_itr->balance, "exchange asset is not equal");
        eosio_assert(game_itr->reserve >= transfer_eos, "insufficient reserve eos");

        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            new_user(account, memo);
            user_itr = users.find(account);
        } else {
            eosio_assert( now() >= user_itr->last_action + _ACTION_COOL_DOWN, "action needs to cool down");
            users.modify(user_itr, 0, [&](auto& u) {
                u.last_action = now();
            });
        }

        asset fee = transfer_eos;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (first round up)
        asset quant_after_fee = transfer_eos;
        quant_after_fee -= fee;

        _game.modify(game_itr, 0, [&](auto& g) {
            g.reserve -= transfer_eos;
            g.balance = token_balance;
            g.circulation = circulation;
        });

        collect_fee(account, fee);

        fee.amount = quant_after_fee.amount;
        if(user_itr->discount == 0){
            fee.amount = (fee.amount + 49) / 50; /// 2% fee (second round up)
        } else {
            fee.amount = (fee.amount + 99) / 100; /// 1% fee (discount half of the fee with a refer)
        }

        asset action_total_fee = fee;
        quant_after_fee -= fee;

        _game.modify(game_itr, 0, [&](auto& g) {
            g.insure += fee / 2;
            g.reward += action_total_fee - fee / 2;
        });

        if(remain_asset > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                GAME_TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, remain_asset, string("refund")))
            .send();
        }

        if(quant_after_fee > asset(0, CORE_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, quant_after_fee, string("sell")))
            .send();
        }
    }

    void burn(account_name account, asset quantity, string memo){
        eosio_assert(quantity.amount >= _UNIT && quantity.amount <= 10000 * _UNIT, "quantity must in range 1 - 10000 CGT");
        
        auto game_itr = _game.begin();
        asset insure_balance = game_itr->insure;
        asset token_supply = game_itr->supply;
        asset token_balance = game_itr->balance;
        asset circulation = token_supply - token_balance;
        real_type token_price = real_type(insure_balance.amount) / real_type(circulation.amount);
        asset transfer_eos = asset(token_price * real_type(quantity.amount), CORE_SYMBOL);

        eosio_assert(transfer_eos <= asset(100 * _UNIT, CORE_SYMBOL) && transfer_eos >= asset(_UNIT, CORE_SYMBOL), "burn in range 1 - 100 eos");
        eosio_assert(insure_balance >= transfer_eos, "insufficient insure eos");

        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            new_user(account, memo);
            user_itr = users.find(account);
        } else {
            eosio_assert( now() >= user_itr->last_action + _ACTION_COOL_DOWN, "action needs to cool down");
            users.modify(user_itr, 0, [&](auto& u) {
                u.last_action = now();
            });
        }

        asset fee = transfer_eos;
        fee.amount = (fee.amount + 99) / 100; /// 1% fee (round up)
        asset quant_after_fee = transfer_eos;
        quant_after_fee -= fee;

        collect_fee(account, fee);

        _game.modify(game_itr, 0, [&](auto& g) {
            g.insure -= transfer_eos;
            g.supply -= quantity;
            g.circulation -= quantity;
            g.burn += quantity;
        });

        if(quantity > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                GAME_TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, BURN_ACCOUNT, quantity, string("burn token")))
            .send();
        }

        if(quant_after_fee > asset(0, CORE_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, quant_after_fee, string("burn return")))
            .send();
        }
    }

    void deposit(account_name account, asset quantity, string memo){
        auto game_itr = _game.begin();
        auto user_itr = users.find(account);
        auto fee_itr = users.find(FEE_ACCOUNT);
        if(quantity.amount >= _REFER_PRICE / 2){
            if(user_itr == users.end()){
                new_user(account, memo);
                user_itr = users.find(account);
            }
            if(quantity.amount >= _REFER_PRICE){
                if(user_itr != users.end()){
                    uint64_t refer = quantity.amount / _REFER_PRICE;
                    users.modify(user_itr, 0, [&](auto& u) {
                        u.refer += refer;
                    });

                    users.modify(fee_itr, 0, [&](auto& u) {
                        u.refer += refer;
                    });
                }
                auto refer_itr = refers.find(account);
                if(refer_itr == refers.end() && user_itr->refer > 0){
                    refer_itr = refers.emplace(account, [&](auto& r){
                        r.name = account;
                    });
                }
                auto refer_fee_itr = refers.find(FEE_ACCOUNT);
                if(refer_fee_itr == refers.end() && fee_itr->refer > 0){
                    refer_fee_itr = refers.emplace(account, [&](auto& r){
                        r.name = FEE_ACCOUNT;
                    });
                }
                auto next_refer_itr = users.find(game_itr->next_refer);
                if(next_refer_itr == users.end()){
                    _game.modify(game_itr, 0, [&](auto& g){
                        g.next_refer = account;
                    });
                }
            }
        }
        
        _game.modify(game_itr, 0, [&](auto& g) {
            g.insure += quantity;
        });
    }


    void stake(account_name account, asset quantity, string memo){
        auto game_itr = _game.begin();
        auto user_itr = users.find(account);
        if(user_itr == users.end()){
            new_user(account, memo);
            user_itr = users.find(account);
        }
        if(account == game_itr->player_one){
            _game.modify(game_itr, 0 , [&](auto& g){
                g.staked += quantity;
            });
        } else if(quantity > game_itr->staked){
            unstake(game_itr->player_one);
            _game.modify(game_itr, 0 , [&](auto& g){
                g.staked = quantity;
                g.reward = asset(0, CORE_SYMBOL);
                g.player_one = account;
            });
        } else {
            eosio_assert(false, "need stake more tokens to overcome player one");
        }
    }

    void unstake(account_name account){
        auto game_itr = _game.begin();
        eosio_assert(game_itr->player_one == account, "can not unstake this account");
        
        asset staked = game_itr->staked;
        asset reward = game_itr->reward;
        _game.modify(game_itr, 0 , [&](auto& g){
            g.staked = asset(0, GAME_SYMBOL);
            g.reward = asset(0, CORE_SYMBOL);
            g.player_one = FEE_ACCOUNT;
        });
        if(staked > asset(0, GAME_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                GAME_TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, staked, string("unstake")))
            .send();
        }
        if(reward > asset(0, CORE_SYMBOL)){
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, account, reward, string("stake reward")))
            .send();
        }
    }


    void new_user(account_name account, string memo){
        auto user_itr = users.find(account);
        if(user_itr != users.end()) return;

        uint32_t discount = 0;
        auto parent = string_to_name(memo.c_str());
        auto parent_itr = users.find(parent);
        auto game_itr = _game.begin();
        if(parent_itr == users.end() || parent_itr->refer <= 0 || account == parent){
            parent = FEE_ACCOUNT;
            parent_itr = users.find(FEE_ACCOUNT);
            auto next_refer_itr = users.find(game_itr->next_refer);
            if(refers.begin() != refers.end() && next_refer_itr != users.end()){
                auto refer_itr = refers.find(game_itr->next_refer);
                if(refer_itr != refers.end()){
                    auto refer_user_itr = users.find(refer_itr->name);
                    if(refer_user_itr != users.end()){
                        parent = refer_user_itr->name;
                        parent_itr = users.find(refer_user_itr->name);
                        if(refer_user_itr->refer <= 1){
                            refer_itr = refers.erase(refer_itr);
                        } else {
                            refer_itr ++;
                        }
                        if(refers.begin() == refers.end()){
                            _game.modify(game_itr, 0, [&](auto& g){
                                g.next_refer = BURN_ACCOUNT;
                            });
                        } else if(refer_itr == refers.end()) {
                            _game.modify(game_itr, 0, [&](auto& g){
                                g.next_refer = refers.begin()->name;
                            });
                        } else {
                            _game.modify(game_itr, 0, [&](auto& g){
                                g.next_refer = refer_itr->name;
                            });
                        }
                    }
                }
            }
        }

        if(parent_itr->refer > 0){
            discount = 1;
            users.modify(parent_itr, 0, [&](auto& u){
                u.refer --;
            });
        }

        users.emplace(account, [&](auto& u) {
            u.name = account;
            u.parent = parent;
            u.discount = discount;
            u.last_action = now();
        });
    }

    void collect_fee(account_name account, asset fee){
        if (fee.amount > 0){
            auto refer_fee = fee;
            refer_fee = fee / 2;
            fee -= refer_fee;

            auto user_itr = users.find(account);
            if(user_itr == users.end()) return;
            auto parent_itr = users.find(user_itr->parent);
            if(parent_itr == users.end()) return;
            users.modify(parent_itr, 0, [&](auto& u){
                u.reward += fee;
            });

            if (refer_fee.amount > 0)
            {
                parent_itr = users.find(parent_itr->parent);
                if(parent_itr == users.end()) return;
                users.modify(parent_itr, 0, [&](auto& u){
                    u.reward += refer_fee;
                });
            }
        }
    }

    void claim_fee(account_name account){
        auto user_itr = users.find(account);
        if(user_itr == users.end()) return;
        if(user_itr->reward > asset(10000ll, CORE_SYMBOL)){
            asset reward = user_itr->reward;
            users.modify(user_itr, 0, [&](auto& u){
                u.reward = asset(0ll, CORE_SYMBOL);
            });
            action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, user_itr->name, reward, string("refer fee")))
            .send();
        }
    }

    private:
    real_type _crr(asset circulation) {
        eosio_assert(circulation.amount >= 0, "shit happens");
        real_type A(_A);
        real_type B(_B);
        real_type L(_L);
        real_type D(_D);
        real_type ONE(1.0);
        real_type H(100.0);
        real_type UNIT(10000.0);
        real_type E(2.71828182845904);

        real_type X(circulation.amount);
        X = X / UNIT;
        
        real_type R = ONE / (ONE + pow(E, (X - L) / D)) * A + B;
        eosio_assert(R >= B && R <= B + A, "shit happens");
        return R / H;
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
        asset staked = asset(0, GAME_SYMBOL);
        asset reward = asset(0, CORE_SYMBOL);
        account_name next_refer = FEE_ACCOUNT;
        account_name player_one = FEE_ACCOUNT;
        uint64_t start_time;

        uint64_t primary_key() const { return gameid; }
        EOSLIB_SERIALIZE(game, (gameid)(reserve)(insure)(max_supply)(supply)(balance)(circulation)(burn)(staked)(reward)(next_refer)(player_one)(start_time))
    };
    typedef eosio::multi_index<N(game), game> game_index;
    game_index _game;

    // @abi table users i64
    struct user{
        account_name name;
        account_name parent;
        asset reward = asset(0, CORE_SYMBOL);
        uint64_t last_action;
        uint32_t refer = 0;
        uint32_t discount = 0;

        uint64_t primary_key() const { return name; }
        EOSLIB_SERIALIZE(user, (name)(parent)(reward)(last_action)(refer)(discount))
    };
    typedef eosio::multi_index<N(users), user> user_index;
    user_index users;

    // @abi table refers i64
    struct refer{
        account_name name;
        uint64_t primary_key() const { return name; }
        EOSLIB_SERIALIZE(refer, (name))
    };
    typedef eosio::multi_index<N(refers), refer> refer_index;
    refer_index refers;
};

#define EOSIO_ABI_EX( TYPE, MEMBERS ) \
extern "C" { \
    void apply( uint64_t receiver, uint64_t code, uint64_t action ) { \
        auto self = receiver; \
        if(action == N(onerror)) { \
            /* onerror is only valid if it is for the "eosio" code account and authorized by "eosio"'s "active permission */ \
            eosio_assert(code == N(eosio), "onerror action's are only valid from the \"eosio\" system account"); \
        } \
        if(action == N(transfer)) { \
            auto _action = N(onerror); \
            if(code == TOKEN_CONTRACT) { \
                _action = N(eosio_token_transfer); \
            } else if(code == GAME_TOKEN_CONTRACT) { \
                _action = N(game_token_transfer); \
            } \
            if(_action == N(onerror)) { \
                eosio_assert(false, "action from this code is denied"); \
            } \
            TYPE thiscontract( self ); \
            switch( _action ) { \
                EOSIO_API( TYPE, MEMBERS ) \
            } \
         /* does not allow destructor of thiscontract to run: eosio_exit(0); */ \
        } \
    } \
} \

EOSIO_ABI_EX(playerone, (eosio_token_transfer)(game_token_transfer))