/******************************************************************************
 * Copyright © 2013-2019 The Komodo Platform Developers.                      *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * Komodo Platform software, including this file may be copied, modified,     *
 * propagated or distributed except according to the terms contained in the   *
 * LICENSE file                                                               *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#if defined(_WIN32) || defined(WIN32)
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>

#    include <wincrypt.h>
#endif

#ifdef __APPLE__
#    include "atomic.dex.osx.manager.hpp"
#    include <QGuiApplication>
#    include <QJsonArray>
#    include <QWindow>
#    include <QWindowList>
#endif

/*#define ENABLE_ENCODER_GENERIC
#include "QZXing.h"*/
//! Project Headers
#include "atomic.dex.app.hpp"
#include "atomic.dex.mm2.hpp"
#include "atomic.dex.provider.cex.prices.hpp"
#include "atomic.dex.provider.coinpaprika.hpp"
#include "atomic.dex.qt.bindings.hpp"
#include "atomic.dex.qt.utilities.hpp"
#include "atomic.dex.security.hpp"
#include "atomic.dex.update.service.hpp"
#include "atomic.dex.utilities.hpp"
#include "atomic.dex.version.hpp"
#include "atomic.threadpool.hpp"

namespace
{
    constexpr std::size_t g_timeout_q_timer_ms = 8;

#if defined(_WIN32) || defined(WIN32)
    bool
    acquire_context(HCRYPTPROV* ctx)
    {
        if (!CryptAcquireContext(ctx, nullptr, nullptr, PROV_RSA_FULL, 0))
        {
            return CryptAcquireContext(ctx, nullptr, nullptr, PROV_RSA_FULL, CRYPT_NEWKEYSET);
        }
        return true;
    }


    size_t
    sysrandom(void* dst, size_t dstlen)
    {
        HCRYPTPROV ctx;
        if (!acquire_context(&ctx))
        {
            throw std::runtime_error("Unable to initialize Win32 crypt library.");
        }

        BYTE* buffer = reinterpret_cast<BYTE*>(dst);
        if (!CryptGenRandom(ctx, dstlen, buffer))
        {
            throw std::runtime_error("Unable to generate random bytes.");
        }

        if (!CryptReleaseContext(ctx, 0))
        {
            throw std::runtime_error("Unable to release Win32 crypt library.");
        }

        return dstlen;
    }
#endif
} // namespace

namespace atomic_dex
{
    void
    atomic_dex::application::change_state([[maybe_unused]] int visibility)
    {
#ifdef __APPLE__
        qDebug() << visibility;
        {
            QWindowList windows = QGuiApplication::allWindows();
            QWindow*    win     = windows.first();
            atomic_dex::mac_window_setup(win->winId(), visibility == QWindow::FullScreen);
        }
#endif
    }

    QVariantList
    atomic_dex::application::get_enabled_coins() const noexcept
    {
        return m_enabled_coins;
    }

    QVariantList
    atomic_dex::application::get_enableable_coins() const noexcept
    {
        return m_enableable_coins;
    }

    bool
    atomic_dex::application::enable_coins(const QStringList& coins)
    {
        std::vector<std::string> coins_std;

        coins_std.reserve(coins.size());
        for (auto&& coin: coins) { coins_std.push_back(coin.toStdString()); }
        atomic_dex::mm2& mm2 = get_mm2();
        mm2.enable_multiple_coins(coins_std);

        return true;
    }

    bool
    application::disable_coins(const QStringList& coins)
    {
        std::vector<std::string> coins_std;

        this->m_portfolio->disable_coins(coins);
        coins_std.reserve(coins.size());
        for (auto&& coin: coins) { coins_std.push_back(coin.toStdString()); }
        get_mm2().disable_multiple_coins(coins_std);
        m_coin_info->set_ticker("");

        return false;
    }

    bool
    atomic_dex::application::first_run()
    {
        return get_wallets().empty();
    }

    void
    application::launch()
    {
        this->system_manager_.start();
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &application::tick);
        timer->start(g_timeout_q_timer_ms);
    }

    QString
    atomic_dex::application::get_mnemonic()
    {
#ifdef __APPLE__
        bc::data_chunk my_entropy_256(32); // 32 bytes = 256 bits

        bc::pseudo_random_fill(my_entropy_256);

        // Instantiate mnemonic word_list
        bc::wallet::word_list words = bc::wallet::create_mnemonic(my_entropy_256);
        return QString::fromStdString(bc::join(words));
#elif defined(_WIN32) || defined(WIN32)
        std::array<unsigned char, WALLY_SECP_RANDOMIZE_LEN> data;
        sysrandom(data.data(), data.size());
        char*  output;
        words* output_words;
        bip39_get_wordlist(NULL, &output_words);
        bip39_mnemonic_from_bytes(output_words, data.data(), data.size(), &output);
        bip39_mnemonic_validate(output_words, output);
        return output;
#else
        std::array<unsigned char, WALLY_SECP_RANDOMIZE_LEN> data{};
        boost::random_device                                device;
        device.generate(data.begin(), data.end());
        char*  output       = nullptr;
        words* output_words = nullptr;
        bip39_get_wordlist(nullptr, &output_words);
        bip39_mnemonic_from_bytes(output_words, data.data(), data.size(), &output);
        bip39_mnemonic_validate(output_words, output);
        return output;
#endif
    }

    void
    application::tick()
    {
        this->process_one_frame();
        if (this->m_need_a_full_refresh_of_mm2)
        {
            auto& mm2_s = system_manager_.create_system<mm2>();
            system_manager_.create_system<coinpaprika_provider>(mm2_s, m_config);
            system_manager_.create_system<cex_prices_provider>(mm2_s);

            connect_signals();
            this->m_need_a_full_refresh_of_mm2 = false;
        }
        auto& mm2     = get_mm2();
        auto& paprika = get_paprika();
        if (mm2.is_mm2_running())
        {
            if (m_coin_info->get_ticker().isEmpty() && not m_enabled_coins.empty())
            {
                // auto coin = mm2.get_enabled_coins().front();
                //! KMD Is our default coin
                m_coin_info->set_ticker("KMD");
                emit coinInfoChanged();
            }

            std::error_code ec;
            auto            fiat_balance_std = paprika.get_price_in_fiat_all(m_config.current_currency, ec);

            if (!ec)
            {
                this->set_current_balance_fiat_all(QString::fromStdString(fiat_balance_std));
            }

            if (not m_coin_info->get_ticker().isEmpty() && not m_enabled_coins.empty())
            {
                refresh_fiat_balance(mm2, paprika);
                refresh_address(mm2);
            }
        }

        if (not this->m_actions_queue.empty())
        {
            action last_action;
            this->m_actions_queue.pop(last_action);
            switch (last_action)
            {
            case action::refresh_enabled_coin:
                if (mm2.is_mm2_running())
                {
                    this->process_refresh_enabled_coin_action();
                }
                break;
            case action::refresh_current_ticker:
                if (mm2.is_mm2_running())
                {
                    this->process_refresh_current_ticker_infos();
                }
                break;
            case action::refresh_ohlc:
                if (mm2.is_mm2_running())
                {
                    emit OHLCDataUpdated();
                }
                break;
            case action::refresh_transactions:
                if (mm2.is_mm2_running())
                {
                    refresh_transactions(mm2);
                }
                break;
            case action::refresh_portfolio_ticker_balance:
                if (mm2.is_mm2_running())
                {
                    this->m_portfolio->update_balance_values(*this->m_ticker_balance_to_refresh);
                }
                break;
            case action::post_process_orders_finished:
                if (mm2.is_mm2_running())
                {
                    this->m_orders->refresh_or_insert_orders();
                }
                break;
            case action::post_process_swaps_finished:
                if (mm2.is_mm2_running())
                {
                    this->m_orders->refresh_or_insert_swaps();
                }
                break;
            case action::refresh_update_status:
                spdlog::trace("refreshing update status in GUI");
                const auto&   update_service_sys = this->system_manager_.get_system<update_system_service>();
                QJsonDocument doc                = QJsonDocument::fromJson(QString::fromStdString(update_service_sys.get_update_status().dump()).toUtf8());
                this->m_update_status            = doc.toVariant();
                emit updateStatusChanged();
                break;
            }
        }
    }

    void
    application::refresh_fiat_balance(const mm2& mm2, const coinpaprika_provider& paprika)
    {
        std::error_code ec;
        QString         target_balance = QString::fromStdString(mm2.my_balance(m_coin_info->get_ticker().toStdString(), ec));
        m_coin_info->set_balance(target_balance);

        if (std::any_of(begin(m_config.possible_currencies), end(m_config.possible_currencies), [this](const std::string& cur_fiat) {
                return cur_fiat == m_config.current_currency;
            }))
        {
            ec          = std::error_code();
            auto amount = QString::fromStdString(paprika.get_price_in_fiat(m_config.current_currency, m_coin_info->get_ticker().toStdString(), ec));
            if (!ec)
            {
                m_coin_info->set_fiat_amount(amount);
            }
        }
    }

    void
    application::refresh_transactions(const mm2& mm2)
    {
        const auto ticker = m_coin_info->get_ticker().toStdString();
        spdlog::debug("{} l{} for coin {}", __FUNCTION__, __LINE__, ticker);
        std::error_code ec;
        auto            txs = mm2.get_tx_history(ticker, ec);
        if (!ec)
        {
            m_coin_info->set_transactions(to_qt_binding(std::move(txs), get_paprika(), m_config.current_currency, ticker));
        }
        auto tx_state = mm2.get_tx_state(ticker, ec);

        if (!ec)
        {
            m_coin_info->set_tx_state(QString::fromStdString(tx_state.state));
            if (mm2.get_coin_info(ticker).is_erc_20)
            {
                m_coin_info->set_blocks_left(tx_state.blocks_left);
            }
            else
            {
                m_coin_info->set_txs_left(tx_state.transactions_left);
            }
            m_coin_info->set_tx_current_block(tx_state.current_block);
        }
    }

    mm2&
    application::get_mm2() noexcept
    {
        return this->system_manager_.get_system<mm2>();
    }

    coinpaprika_provider&
    application::get_paprika() noexcept
    {
        return this->system_manager_.get_system<coinpaprika_provider>();
    }

    entt::dispatcher&
    application::get_dispatcher() noexcept
    {
        return this->dispatcher_;
    }

    QObject*
    atomic_dex::application::get_current_coin_info() const noexcept
    {
        return m_coin_info;
    }

    QString
    atomic_dex::application::get_balance_fiat_all() const noexcept
    {
        return m_current_balance_all;
    }

    QString
    application::get_second_balance_fiat_all() const noexcept
    {
        return m_second_current_balance_all;
    }

    void
    atomic_dex::application::set_current_balance_fiat_all(QString current_fiat_all_balance) noexcept
    {
        this->m_current_balance_all = std::move(current_fiat_all_balance);
        emit on_fiat_balance_all_changed();
    }

    void
    application::set_second_current_balance_fiat_all(QString current_fiat_all_balance) noexcept
    {
        this->m_second_current_balance_all = std::move(current_fiat_all_balance);
        emit on_second_fiat_balance_all_changed();
    }

    application::application(QObject* pParent) noexcept :
        QObject(pParent),
        m_update_status(QJsonObject{
            {"update_needed", false}, {"changelog", ""}, {"current_version", ""}, {"download_url", ""}, {"new_version", ""}, {"rpc_code", 0}, {"status", ""}}),
        m_coin_info(new current_coin_info(dispatcher_, this)), m_addressbook(new addressbook_model(this->m_wallet_manager, this)),
        m_portfolio(new portfolio_model(this->system_manager_, this->m_config, this)), m_orders(new orders_model(this->system_manager_, this))
    {
        get_dispatcher().sink<refresh_update_status>().connect<&application::on_refresh_update_status_event>(*this);
        //! MM2 system need to be created before the GUI and give the instance to the gui
        auto& mm2_system = system_manager_.create_system<mm2>();
        system_manager_.create_system<coinpaprika_provider>(mm2_system, m_config);
        system_manager_.create_system<cex_prices_provider>(mm2_system);
        system_manager_.create_system<update_system_service>();

        connect_signals();
        if (is_there_a_default_wallet())
        {
            set_wallet_default_name(get_default_wallet_name());
        }
    }

    void
    atomic_dex::application::cancel_order(const QString& order_id)
    {
        auto& mm2 = get_mm2();
        atomic_dex::spawn([&mm2, order_id]() {
            ::mm2::api::rpc_cancel_order({order_id.toStdString()});
            mm2.process_orders();
        });
    }

    void
    atomic_dex::application::cancel_all_orders()
    {
        auto& mm2 = get_mm2();
        atomic_dex::spawn([&mm2]() {
            ::mm2::api::cancel_all_orders_request req;
            ::mm2::api::rpc_cancel_all_orders(std::move(req));
            mm2.process_orders();
        });
    }

    void
    application::cancel_all_orders_by_ticker(const QString& ticker)
    {
        auto& mm2 = get_mm2();
        atomic_dex::spawn([&mm2, &ticker]() {
            ::mm2::api::cancel_data cd;
            cd.ticker = ticker.toStdString();
            ::mm2::api::cancel_all_orders_request req{{"Coin", cd}};
            ::mm2::api::rpc_cancel_all_orders(std::move(req));
            mm2.process_orders();
        });
    }

    void
    atomic_dex::application::on_enabled_coins_event([[maybe_unused]] const enabled_coins_event& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_enabled_coin);
    }

    void
    application::on_enabled_default_coins_event([[maybe_unused]] const enabled_default_coins_event& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_enabled_coin);
    }

    void
    application::on_coin_fully_initialized_event(const coin_fully_initialized& evt) noexcept
    {
        //! This event is called when a call is enabled and cex provider finished fetch datas
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_portfolio->initialize_portfolio(evt.ticker);
    }

    QString
    application::get_current_currency() const noexcept
    {
        return QString::fromStdString(this->m_config.current_currency);
    }

    void
    application::set_current_currency(const QString& current_currency) noexcept
    {
        if (current_currency.toStdString() != m_config.current_currency)
        {
            spdlog::info("change currency {} to {}", m_config.current_currency, current_currency.toStdString());
            atomic_dex::change_currency(m_config, current_currency.toStdString());
            this->m_portfolio->update_currency_values();
            emit on_currency_changed();
        }
    }

    QString
    application::get_current_fiat() const noexcept
    {
        return QString::fromStdString(this->m_config.current_fiat);
    }

    void
    application::set_current_fiat(const QString& current_fiat) noexcept
    {
        if (current_fiat.toStdString() != m_config.current_fiat)
        {
            spdlog::info("change fiat {} to {}", m_config.current_fiat, current_fiat.toStdString());
            atomic_dex::change_fiat(m_config, current_fiat.toStdString());
            emit on_fiat_changed();
        }
    }

    void
    application::on_change_ticker_event([[maybe_unused]] const change_ticker_event& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_current_ticker);
    }

    void
    application::refresh_address(mm2& mm2)
    {
        std::error_code ec;
        auto            address = QString::fromStdString(mm2.address(m_coin_info->get_ticker().toStdString(), ec));
        this->m_coin_info->set_address(address);
    }

    QObject*
    application::prepare_send(const QString& address, const QString& amount, bool max)
    {
        atomic_dex::t_withdraw_request req{
            .coin = m_coin_info->get_ticker().toStdString(), .to = address.toStdString(), .amount = amount.toStdString(), .max = max};
        if (req.max)
        {
            req.amount = "0";
        }
        std::error_code ec;
        auto            answer = mm2::withdraw(std::move(req), ec);
        auto            coin   = get_mm2().get_coin_info(m_coin_info->get_ticker().toStdString());
        return to_qt_binding(std::move(answer), this, QString::fromStdString(coin.explorer_url[0]));
    }

    QObject*
    application::prepare_send_fees(
        const QString& address, const QString& amount, bool is_erc_20, const QString& fees_amount, const QString& gas_price, const QString& gas, bool max)
    {
        atomic_dex::t_withdraw_request req{
            .coin = m_coin_info->get_ticker().toStdString(), .to = address.toStdString(), .amount = amount.toStdString(), .max = max};
        if (req.max)
        {
            req.amount = "0";
        }
        req.fees = atomic_dex::t_withdraw_fees{
            .type      = is_erc_20 ? "EthGas" : "UtxoFixed",
            .amount    = fees_amount.toStdString(),
            .gas_price = gas_price.toStdString(),
            .gas_limit = not gas.isEmpty() ? std::stoi(gas.toStdString()) : 0};
        std::error_code ec;
        auto            answer = mm2::withdraw(std::move(req), ec);
        auto            coin   = get_mm2().get_coin_info(m_coin_info->get_ticker().toStdString());
        return to_qt_binding(std::move(answer), this, QString::fromStdString(coin.explorer_url[0]));
    }

    bool
    atomic_dex::application::is_claiming_ready(const QString& ticker)
    {
        return get_mm2().is_claiming_ready(ticker.toStdString());
    }

    QObject*
    atomic_dex::application::claim_rewards(const QString& ticker)
    {
        std::error_code ec;
        auto            answer = get_mm2().claim_rewards(ticker.toStdString(), ec);

        if (not answer.error.has_value())
        {
            if (ec)
            {
                answer.error = ec.message();
            }
        }

        const auto coin = get_mm2().get_coin_info(m_coin_info->get_ticker().toStdString());
        QObject*   obj  = to_qt_binding(std::move(answer), this, QString::fromStdString(coin.explorer_url[0]));

        return obj;
    }

    QString
    application::send(const QString& tx_hex)
    {
        atomic_dex::t_broadcast_request req{.tx_hex = tx_hex.toStdString(), .coin = m_coin_info->get_ticker().toStdString()};
        std::error_code                 ec;
        auto                            answer = get_mm2().broadcast(std::move(req), ec);
        this->m_actions_queue.push(action::refresh_current_ticker);
        refresh_infos();
        return QString::fromStdString(answer.tx_hash);
    }

    QString
    application::send_rewards(const QString& tx_hex)
    {
        atomic_dex::t_broadcast_request req{.tx_hex = tx_hex.toStdString(), .coin = m_coin_info->get_ticker().toStdString()};
        std::error_code                 ec;
        auto                            answer = get_mm2().send_rewards(std::move(req), ec);
        this->m_actions_queue.push(action::refresh_current_ticker);
        refresh_infos();
        return QString::fromStdString(answer.tx_hash);
    }

    void
    application::on_tx_fetch_finished_event([[maybe_unused]] const tx_fetch_finished& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_transactions);
    }

    bool
    application::place_buy_order(const QString& base, const QString& rel, const QString& price, const QString& volume)
    {
        t_float_50 price_f;
        t_float_50 amount_f;
        t_float_50 total_amount;

        price_f.assign(price.toStdString());
        amount_f.assign(volume.toStdString());
        total_amount = price_f * amount_f;

        t_buy_request   req{.base = base.toStdString(), .rel = rel.toStdString(), .price = price.toStdString(), .volume = volume.toStdString()};
        std::error_code ec;
        auto            answer = get_mm2().place_buy_order(std::move(req), total_amount, ec);

        return !answer.error.has_value();
    }

    QString
    application::place_sell_order(
        const QString& base, const QString& rel, const QString& price, const QString& volume, bool is_created_order, const QString& price_denom,
        const QString& price_numer)
    {
        qDebug() << " base: " << base << " rel: " << rel << " price: " << price << " volume: " << volume;
        t_float_50 amount_f;
        amount_f.assign(volume.toStdString());

        t_sell_request req{
            .base             = base.toStdString(),
            .rel              = rel.toStdString(),
            .price            = price.toStdString(),
            .volume           = volume.toStdString(),
            .is_created_order = is_created_order,
            .price_denom      = price_denom.toStdString(),
            .price_numer      = price_numer.toStdString()};
        std::error_code ec;
        auto            answer = get_mm2().place_sell_order(std::move(req), amount_f, ec);

        if (answer.error.has_value())
        {
            return QString::fromStdString(answer.error.value());
        }
        return "";
    }

    bool
    application::do_i_have_enough_funds(const QString& ticker, const QString& amount) const
    {
        t_float_50 amount_f(amount.toStdString());
        return get_mm2().do_i_have_enough_funds(ticker.toStdString(), amount_f);
    }

    const mm2&
    application::get_mm2() const noexcept
    {
        return this->system_manager_.get_system<mm2>();
    }

    void
    application::on_coin_disabled_event([[maybe_unused]] const coin_disabled& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_enabled_coin);
        // m_refresh_enabled_coin_event = true;
    }

    QString
    application::get_balance(const QString& coin)
    {
        std::error_code ec;
        auto            res = get_mm2().my_balance(coin.toStdString(), ec);
        return QString::fromStdString(res);
    }

    void
    application::on_gui_enter_dex()
    {
        this->dispatcher_.trigger<gui_enter_trading>();
    }

    void
    application::on_gui_leave_dex()
    {
        this->dispatcher_.trigger<gui_leave_trading>();
    }

    QString
    application::get_status() const noexcept
    {
        return m_current_status;
    }

    void
    application::set_status(QString status) noexcept
    {
        this->m_current_status = std::move(status);
        emit on_status_changed();
    }

    void
    application::on_mm2_initialized_event([[maybe_unused]] const mm2_initialized& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->set_status("enabling_coins");
    }

    void
    application::on_mm2_started_event([[maybe_unused]] const mm2_started& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->set_status("complete");
    }

    void
    application::set_current_orderbook(const QString& base, const QString& rel)
    {
        this->dispatcher_.trigger<orderbook_refresh>(base.toStdString(), rel.toStdString());
    }

    QVariantMap
    application::get_orderbook(const QString& ticker)
    {
        QVariantMap     out;
        std::error_code ec;
        auto            answer = get_mm2().get_orderbook(ticker.toStdString(), ec);
        if (ec == dextop_error::orderbook_ticker_not_found)
        {
            spdlog::warn("{}", ec.message());
            return out;
        }
        for (auto&& current_orderbook: answer)
        {
            nlohmann::json j_out = nlohmann::json::array();
            for (auto&& current_bid: current_orderbook.bids)
            {
                nlohmann::json current_j_bid = {
                    {"volume", current_bid.maxvolume},
                    {"price", current_bid.price},
                    {"price_denom", current_bid.price_fraction_denom},
                    {"price_numer", current_bid.price_fraction_numer}};
                j_out.push_back(current_j_bid);
            }
            auto out_orderbook = QJsonDocument::fromJson(QString::fromStdString(j_out.dump()).toUtf8());
            out.insert(QString::fromStdString(current_orderbook.rel), out_orderbook.toVariant());
        }
        return out;
    }

    void
    application::on_refresh_update_status_event([[maybe_unused]] const refresh_update_status& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_update_status);
    }

    void
    application::refresh_infos()
    {
        auto& mm2 = get_mm2();
        spawn([&mm2]() { mm2.fetch_infos_thread(); });
    }

    void
    application::refresh_orders_and_swaps()
    {
        auto& mm2 = get_mm2();
        spawn([&mm2]() {
            mm2.process_swaps();
            mm2.process_orders();
        });
    }

    QVariant
    application::get_coin_info(const QString& ticker)
    {
        QVariant       out;
        nlohmann::json j      = to_qt_binding(get_mm2().get_coin_info(ticker.toStdString()));
        QJsonDocument  q_json = QJsonDocument::fromJson(QString::fromStdString(j.dump()).toUtf8());
        out                   = q_json.toVariant();
        return out;
    }

    bool
    application::disconnect()
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);

        //! Clear pending events
        while (not this->m_actions_queue.empty())
        {
            [[maybe_unused]] action act;
            this->m_actions_queue.pop(act);
        }

        //! Clear models
        if (auto count = this->m_addressbook->rowCount(); count > 0)
        {
            this->m_addressbook->removeRows(0, count);
        }

        if (auto count = this->m_portfolio->rowCount(QModelIndex()); count > 0)
        {
            this->m_portfolio->removeRows(0, count, QModelIndex());
        }

        if (auto count = this->m_orders->rowCount(QModelIndex()); count > 0)
        {
            this->m_orders->removeRows(0, count, QModelIndex());
        }
        this->m_orders->clear_registry();

        //! Mark systems
        system_manager_.mark_system<mm2>();
        system_manager_.mark_system<coinpaprika_provider>();
        system_manager_.mark_system<cex_prices_provider>();

        //! Disconnect signals
        get_dispatcher().sink<ticker_balance_updated>().disconnect<&application::on_ticker_balance_updated_event>(*this);
        get_dispatcher().sink<change_ticker_event>().disconnect<&application::on_change_ticker_event>(*this);
        get_dispatcher().sink<enabled_coins_event>().disconnect<&application::on_enabled_coins_event>(*this);
        get_dispatcher().sink<enabled_default_coins_event>().disconnect<&application::on_enabled_default_coins_event>(*this);
        get_dispatcher().sink<coin_fully_initialized>().disconnect<&application::on_coin_fully_initialized_event>(*this);
        get_dispatcher().sink<tx_fetch_finished>().disconnect<&application::on_tx_fetch_finished_event>(*this);
        get_dispatcher().sink<coin_disabled>().disconnect<&application::on_coin_disabled_event>(*this);
        get_dispatcher().sink<mm2_initialized>().disconnect<&application::on_mm2_initialized_event>(*this);
        get_dispatcher().sink<mm2_started>().disconnect<&application::on_mm2_started_event>(*this);
        get_dispatcher().sink<refresh_ohlc_needed>().disconnect<&application::on_refresh_ohlc_event>(*this);
        get_dispatcher().sink<process_orders_finished>().disconnect<&application::on_process_orders_finished_event>(*this);
        get_dispatcher().sink<process_swaps_finished>().disconnect<&application::on_process_swaps_finished_event>(*this);

        this->m_need_a_full_refresh_of_mm2 = true;

        return fs::remove(get_atomic_dex_config_folder() / "default.wallet");
    }

    void
    application::connect_signals()
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        get_dispatcher().sink<ticker_balance_updated>().connect<&application::on_ticker_balance_updated_event>(*this);
        get_dispatcher().sink<change_ticker_event>().connect<&application::on_change_ticker_event>(*this);
        get_dispatcher().sink<enabled_coins_event>().connect<&application::on_enabled_coins_event>(*this);
        get_dispatcher().sink<enabled_default_coins_event>().connect<&application::on_enabled_default_coins_event>(*this);
        get_dispatcher().sink<coin_fully_initialized>().connect<&application::on_coin_fully_initialized_event>(*this);
        get_dispatcher().sink<tx_fetch_finished>().connect<&application::on_tx_fetch_finished_event>(*this);
        get_dispatcher().sink<coin_disabled>().connect<&application::on_coin_disabled_event>(*this);
        get_dispatcher().sink<mm2_initialized>().connect<&application::on_mm2_initialized_event>(*this);
        get_dispatcher().sink<mm2_started>().connect<&application::on_mm2_started_event>(*this);
        get_dispatcher().sink<refresh_ohlc_needed>().connect<&application::on_refresh_ohlc_event>(*this);
        get_dispatcher().sink<process_orders_finished>().connect<&application::on_process_orders_finished_event>(*this);
        get_dispatcher().sink<process_swaps_finished>().connect<&application::on_process_swaps_finished_event>(*this);
    }

    QString
    atomic_dex::application::get_regex_password_policy() noexcept
    {
        return QString(::atomic_dex::get_regex_password_policy());
    }

    QVariantMap
    application::get_trade_infos(const QString& ticker, const QString& receive_ticker, const QString& amount)
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        QVariantMap out;

        t_float_50 trade_fee_f = get_mm2().get_trade_fee(ticker.toStdString(), amount.toStdString(), false);
        auto       answer      = get_mm2().get_trade_fixed_fee(ticker.toStdString());

        if (!answer.amount.empty())
        {
            t_float_50 erc_fees = 0;
            t_float_50 tx_fee_f = t_float_50(answer.amount) * 2;

            if (receive_ticker != "")
            {
                get_mm2().apply_erc_fees(receive_ticker.toStdString(), erc_fees);
            }

            auto tx_fee_value = QString::fromStdString(get_formated_float(tx_fee_f));

            const std::string amount_std      = t_float_50(amount.toStdString()) < minimal_trade_amount() ? minimal_trade_amount_str() : amount.toStdString();
            t_float_50        final_balance_f = t_float_50(amount_std) - (trade_fee_f + tx_fee_f);
            std::string       final_balance   = amount.toStdString();
            if (final_balance_f.convert_to<float>() > 0.0)
            {
                final_balance = get_formated_float(final_balance_f);
                out.insert("not_enough_balance_to_pay_the_fees", false);
            }
            else
            {
                out.insert("not_enough_balance_to_pay_the_fees", true);
                t_float_50 amount_needed = minimal_trade_amount() - final_balance_f;
                out.insert("amount_needed", QString::fromStdString(get_formated_float(amount_needed)));
            }
            auto final_balance_qt = QString::fromStdString(final_balance);

            out.insert("trade_fee", QString::fromStdString(get_mm2().get_trade_fee_str(ticker.toStdString(), amount.toStdString(), false)));
            out.insert("tx_fee", tx_fee_value);
            if (erc_fees > 0)
            {
                auto erc_value = QString::fromStdString(get_formated_float(erc_fees));
                out.insert("erc_fees", erc_value);
            }
            out.insert("is_ticker_of_fees_eth", get_mm2().get_coin_info(ticker.toStdString()).is_erc_20);
            out.insert("input_final_value", final_balance_qt);
        }
        return out;
    }

    QString
    application::get_current_lang() const noexcept
    {
        return m_current_lang;
    }

    void
    application::set_current_lang(const QString& current_lang) noexcept
    {
        this->m_current_lang = current_lang;
        if (m_config.current_lang != current_lang.toStdString())
        {
            change_lang(m_config, current_lang.toStdString());
        }
        auto get_locale = [](const QString& current_lang) {
            if (current_lang == "tr")
            {
                return QLocale::Language::Turkish;
            }
            if (current_lang == "en")
            {
                return QLocale::Language::English;
            }
            if (current_lang == "fr")
            {
                return QLocale::Language::French;
            }
            return QLocale::Language::AnyLanguage;
        };

        qDebug() << "locale before: " << QLocale().name();
        QLocale::setDefault(get_locale(current_lang));
        qDebug() << "locale after: " << QLocale().name();
        [[maybe_unused]] auto res = this->m_translator.load("atomic_qt_" + current_lang, QLatin1String(":/atomic_qt_design/assets/languages"));
        assert(res);
        this->m_app->installTranslator(&m_translator);
        emit on_lang_changed();
        emit lang_changed();
    }

    void
    application::set_qt_app(std::shared_ptr<QApplication> app) noexcept
    {
        this->m_app = app;
        set_current_lang(QString::fromStdString(m_config.current_lang));
    }

    QStringList
    application::get_available_langs() const
    {
        QStringList out;
        out.reserve(m_config.available_lang.size());
        for (auto&& cur_lang: m_config.available_lang) { out.push_back(QString::fromStdString(cur_lang)); }
        return out;
    }

    QStringList
    application::get_available_fiats() const
    {
        QStringList out;
        out.reserve(m_config.available_fiat.size());
        for (auto&& cur_fiat: m_config.available_fiat) { out.push_back(QString::fromStdString(cur_fiat)); }
        return out;
    }

    QStringList
    application::get_available_currencies() const
    {
        QStringList out;
        out.reserve(m_config.possible_currencies.size());
        for (auto&& cur_currency: m_config.possible_currencies) { out.push_back(QString::fromStdString(cur_currency)); }
        return out;
    }

    const QString&
    application::get_empty_string()
    {
        static const QString empty_string = "";
        return empty_string;
    }

    QString
    application::retrieve_seed(const QString& wallet_name, const QString& password)
    {
        std::error_code ec;
        auto            key = atomic_dex::derive_password(password.toStdString(), ec);
        if (ec)
        {
            spdlog::warn("{}", ec.message());
            if (ec == dextop_error::derive_password_failed)
            {
                return "wrong password";
            }
        }
        using namespace std::string_literals;
        const fs::path seed_path = get_atomic_dex_config_folder() / (wallet_name.toStdString() + ".seed"s);
        auto           seed      = atomic_dex::decrypt(seed_path, key.data(), ec);
        if (ec == dextop_error::corrupted_file_or_wrong_password)
        {
            spdlog::warn("{}", ec.message());
            return "wrong password";
        }
        return QString::fromStdString(seed);
    }


    bool
    application::mnemonic_validate(const QString& entropy)
    {
#ifdef __APPLE__
        std::vector<std::string> mnemonic;

        // Split
        std::string       s         = entropy.toStdString();
        const std::string delimiter = " ";
        size_t            pos       = 0;
        while ((pos = s.find(delimiter)) != std::string::npos)
        {
            mnemonic.emplace_back(s.substr(0, pos));
            s.erase(0, pos + delimiter.length());
        }
        mnemonic.emplace_back(s);

        // Validate
        return bc::wallet::validate_mnemonic(mnemonic);
#else
        return bip39_mnemonic_validate(nullptr, entropy.toStdString().c_str()) == 0;
#endif
    }

    bool
    application::export_swaps_json() noexcept
    {
        auto swaps = get_mm2().get_swaps().raw_result;

        if (not swaps.empty())
        {
            auto export_file_path = get_atomic_dex_current_export_recent_swaps_file();

            std::ofstream ofs(export_file_path.string(), std::ios::out | std::ios::trunc);
            auto          j = nlohmann::json::parse(swaps);
            ofs << std::setw(4) << j;
            ofs.close();
            return true;
        }
        return false;
    }

    bool
    application::export_swaps(const QString& csv_filename) noexcept
    {
        auto           swaps    = get_mm2().get_swaps();
        const fs::path csv_path = get_atomic_dex_export_folder() / (csv_filename.toStdString() + std::string(".csv"));

        std::ofstream ofs(csv_path.string(), std::ios::out | std::ios::trunc);
        ofs << "Maker Coin, Taker Coin, Maker Amount, Taker Amount, Type, Events, My Info, Is Recoverable" << std::endl;
        for (auto&& swap: swaps.swaps)
        {
            ofs << swap.maker_coin << ",";
            ofs << swap.taker_coin << ",";
            ofs << swap.maker_amount << ",";
            ofs << swap.taker_amount << ",";
            ofs << swap.type << ",";
            ofs << "not supported yet,"; //! This contains many events, what do we want to export here?
            ofs << "not supported yet,"; //! This is a big json object, need to choose which information we need to export ?
            ofs << (swap.funds_recoverable ? "True," : "False,");
            ofs << std::endl;
        }
        ofs.close();
        return true;
    }

    QString
    application::recover_fund(const QString& uuid)
    {
        QString result;

        ::mm2::api::recover_funds_of_swap_request request{.swap_uuid = uuid.toStdString()};
        auto                                      res = ::mm2::api::rpc_recover_funds(std::move(request));
        result                                        = QString::fromStdString(res.raw_result);

        return result;
    }

    QString
    application::get_price_amount(const QString& base_amount, const QString& rel_amount)
    {
        t_float_50 base_amount_f(base_amount.toStdString());
        t_float_50 rel_amount_f(rel_amount.toStdString());
        auto       final = (rel_amount_f / base_amount_f);

        std::stringstream ss;
        ss << std::fixed << std::setprecision(50) << final;
        spdlog::info("base_amount = {}, rel_amount = {}, final_amount = {}", base_amount.toStdString(), rel_amount.toStdString(), ss.str());
        return QString::fromStdString(ss.str());
    }
} // namespace atomic_dex

//! Constructor / Destructor
namespace atomic_dex
{
    application::~application() noexcept
    {
        if (this->m_addressbook->rowCount() > 0)
        {
            this->m_addressbook->removeRows(0, this->m_addressbook->rowCount());
        }
        export_swaps_json();
    }
} // namespace atomic_dex

//! Misc QML Utilities
namespace atomic_dex
{
    QVariant
    application::get_update_status() const noexcept
    {
        return m_update_status;
    }

    QVariantList
    application::get_all_coins() const noexcept
    {
        return to_qt_binding(get_mm2().get_all_coins());
    }

    QString
    application::get_paprika_id_from_ticker(const QString& ticker) const
    {
        return QString::fromStdString(get_mm2().get_coin_info(ticker.toStdString()).coinpaprika_id);
    }

    QString
    application::get_version() noexcept
    {
        return QString::fromStdString(atomic_dex::get_version());
    }

    QString
    application::get_log_folder()
    {
        return QString::fromStdString(get_atomic_dex_logs_folder().string());
    }

    QString
    application::get_mm2_version()
    {
        return QString::fromStdString(::mm2::api::rpc_version());
    }

    QString
    application::get_export_folder()
    {
        return QString::fromStdString(get_atomic_dex_export_folder().string());
    }

    QString
    application::to_eth_checksum_qt(const QString& eth_lowercase_address)
    {
        auto str = eth_lowercase_address.toStdString();
        to_eth_checksum(str);
        return QString::fromStdString(str);
    }
} // namespace atomic_dex

//! Trading functions
namespace atomic_dex
{
    QString
    application::get_cex_rates(const QString& base, const QString& rel)
    {
        std::error_code ec;
        return QString::fromStdString(get_paprika().get_cex_rates(base.toStdString(), rel.toStdString(), ec));
    }

    QString
    application::get_fiat_from_amount(const QString& ticker, const QString& amount)
    {
        std::error_code ec;
        return QString::fromStdString(get_paprika().get_price_as_currency_from_amount(m_config.current_fiat, ticker.toStdString(), amount.toStdString(), ec));
    }
} // namespace atomic_dex

//! OHLC Relative functions
namespace atomic_dex
{
    QVariantList
    application::get_ohlc_data(const QString& range)
    {
        QVariantList out;
        auto&        provider = this->system_manager_.get_system<cex_prices_provider>();
        auto         json     = provider.get_ohlc_data(range.toStdString());

        QJsonDocument q_json = QJsonDocument::fromJson(QString::fromStdString(json.dump()).toUtf8());
        out                  = q_json.array().toVariantList();
        return out;
    }

    QVariantMap
    application::find_closest_ohlc_data(int range, int timestamp)
    {
        QVariantMap out;
        auto&       provider = this->system_manager_.get_system<cex_prices_provider>();
        auto        json     = provider.get_ohlc_data(std::to_string(range));

        auto it = std::lower_bound(rbegin(json), rend(json), timestamp, [](const nlohmann::json& current_json, int timestamp) {
            int res = current_json.at("timestamp").get<int>();
            return timestamp < res;
        });

        if (it != json.rend())
        {
            QJsonDocument q_json = QJsonDocument::fromJson(QString::fromStdString(it->dump()).toUtf8());
            out                  = q_json.object().toVariantMap();
        }
        return out;
    }

    bool
    application::is_supported_ohlc_data_ticker_pair(const QString& base, const QString& rel)
    {
        auto& provider = this->system_manager_.get_system<cex_prices_provider>();
        auto [normal, quoted] =  provider.is_pair_supported(base.toStdString(), rel.toStdString());
        return normal & quoted;
    }

    void
    application::on_refresh_ohlc_event([[maybe_unused]] const refresh_ohlc_needed& evt) noexcept
    {
        spdlog::debug("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_ohlc);
    }

    void
    application::on_ticker_balance_updated_event(const ticker_balance_updated& evt) noexcept
    {
        spdlog::trace("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::refresh_portfolio_ticker_balance);
        *this->m_ticker_balance_to_refresh = evt.ticker;
    }
} // namespace atomic_dex

//! Addressbook
namespace atomic_dex
{
    addressbook_model*
    application::get_addressbook() const noexcept
    {
        return m_addressbook;
    }
} // namespace atomic_dex

//! Orders
namespace atomic_dex
{
    void
    application::on_process_swaps_finished_event([[maybe_unused]] const process_swaps_finished& evt) noexcept
    {
        spdlog::trace("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::post_process_swaps_finished);
    }

    void
    application::on_process_orders_finished_event([[maybe_unused]] const process_orders_finished& evt) noexcept
    {
        spdlog::trace("{} l{}", __FUNCTION__, __LINE__);
        this->m_actions_queue.push(action::post_process_orders_finished);
    }

    orders_model*
    application::get_orders() const noexcept
    {
        return m_orders;
    }
} // namespace atomic_dex

//! Portfolio
namespace atomic_dex
{
    portfolio_model*
    application::get_portfolio() const noexcept
    {
        return m_portfolio;
    }
} // namespace atomic_dex

//! Wallet manager QML API
namespace atomic_dex
{
    QString
    application::get_wallet_default_name() const noexcept
    {
        return m_wallet_manager.get_wallet_default_name();
    }

    void
    application::set_wallet_default_name(QString wallet_name) noexcept
    {
        m_wallet_manager.set_wallet_default_name(std::move(wallet_name));
        emit on_wallet_default_name_changed();
    }

    bool
    atomic_dex::application::create(const QString& password, const QString& seed, const QString& wallet_name)
    {
        return m_wallet_manager.create(password, seed, wallet_name);
    }

    bool
    application::login(const QString& password, const QString& wallet_name)
    {
        bool res = m_wallet_manager.login(password, wallet_name, get_mm2(), [this]() { this->set_status("initializing_mm2"); });
        this->m_addressbook->initializeFromCfg();
        return res;
    }

    bool
    application::confirm_password(const QString& wallet_name, const QString& password)
    {
        return atomic_dex::qt_wallet_manager::confirm_password(wallet_name, password);
    }

    bool
    application::delete_wallet(const QString& wallet_name)
    {
        return qt_wallet_manager::delete_wallet(wallet_name);
    }

    QString
    application::get_default_wallet_name()
    {
        return atomic_dex::qt_wallet_manager::get_default_wallet_name();
    }

    QStringList
    application::get_wallets()
    {
        return atomic_dex::qt_wallet_manager::get_wallets();
    }

    bool
    application::is_there_a_default_wallet()
    {
        return atomic_dex::qt_wallet_manager::is_there_a_default_wallet();
    }
} // namespace atomic_dex

//! Actions implementation
namespace atomic_dex
{
    void
    application::process_refresh_enabled_coin_action()
    {
        auto& mm2          = get_mm2();
        auto  refresh_coin = [](t_coins coins_container, auto&& coins_list) {
            coins_list.clear();
            coins_list = to_qt_binding(std::move(coins_container));
        };

        {
            auto coins = mm2.get_enabled_coins();
            refresh_coin(coins, m_enabled_coins);
            emit enabledCoinsChanged();
        }
        {
            auto coins = mm2.get_enableable_coins();
            refresh_coin(coins, m_enableable_coins);
            emit enableableCoinsChanged();
        }
        {
            if (not m_coin_info->get_ticker().isEmpty())
            {
                refresh_transactions(mm2);
            }
        }
    }

    void
    application::process_refresh_current_ticker_infos()
    {
        auto& mm2     = get_mm2();
        auto& paprika = get_paprika();

        refresh_transactions(mm2);
        refresh_fiat_balance(mm2, paprika);
        refresh_address(mm2);
        {
            const auto  ticker = m_coin_info->get_ticker().toStdString();
            const auto& info   = get_mm2().get_coin_info(ticker);
            m_coin_info->set_name(QString::fromStdString(info.name));
            m_coin_info->set_claimable(info.is_claimable);
            m_coin_info->set_type(QString::fromStdString(info.type));
            m_coin_info->set_paprika_id(QString::fromStdString(info.coinpaprika_id));
            m_coin_info->set_minimal_balance_for_asking_rewards(QString::fromStdString(info.minimal_claim_amount));
            m_coin_info->set_explorer_url(QString::fromStdString(info.explorer_url[0]));
            std::error_code ec;
            m_coin_info->set_price(QString::fromStdString(paprika.get_rate_conversion(m_config.current_currency, ticker, ec, true)));
            m_coin_info->set_change24h(retrieve_change_24h(paprika, info, m_config));
            m_coin_info->set_trend_7d(nlohmann_json_array_to_qt_json_array(paprika.get_ticker_historical(ticker).answer));
        }
    }
} // namespace atomic_dex