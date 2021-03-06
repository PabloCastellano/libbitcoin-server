/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-server.
 *
 * libbitcoin-server is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/message/notifier.hpp>

#include <cstdint>
#include <boost/date_time.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/server/configuration.hpp>
#include <bitcoin/server/settings.hpp>

namespace libbitcoin {
namespace server {

using namespace bc::chain;
using namespace bc::wallet;

#define NAME "notifier"

const auto now = []()
{
    return boost::posix_time::second_clock::universal_time();
};

notifier::notifier(server_node::ptr node)
  : threadpool_(node->server_settings().threads),
    dispatch_(threadpool_, NAME),
    settings_(node->server_settings())
{
    const auto receive_block = [this](uint32_t height, const block::ptr block)
    {
        const auto hash = block->header.hash();
        for (const auto& tx: block->transactions)
            scan(height, hash, tx);
    };

    const auto receive_tx = [this](const transaction& tx)
    {
        scan(0, null_hash, tx);
    };

    // Subscribe against the node's tx and block publishers.
    // This allows the subscription manager to capture transactions from both
    // contexts and search them for payment addresses that match subscriptions.
    node->subscribe_blocks(receive_block);
    node->subscribe_transactions(receive_tx);
}

// ----------------------------------------------------------------------------
// Start sequence.

bool notifier::start()
{
    if (settings_.subscription_limit > 0)
    {
        threadpool_.join();
        threadpool_.spawn(settings_.threads, thread_priority::low);
    }

    // This is the end of the start sequence.
    return true;
}

// ----------------------------------------------------------------------------
// Subscribe sequence.

void notifier::subscribe(const incoming& request, send_handler handler)
{
    dispatch_.ordered(
        &notifier::do_subscribe,
            shared_from_this(), request, handler);
}

void notifier::do_subscribe(const incoming& request, send_handler handler)
{
    const auto ec = add(request, handler);

    // Send response.
    data_chunk result(sizeof(uint32_t));
    auto serial = make_serializer(result.begin());
    serial.write_error_code(ec);
    outgoing response(request, result);

    // This is the end of the subscribe sequence.
    handler(response);
}

// ----------------------------------------------------------------------------
// Renew sequence.

void notifier::renew(const incoming& request, send_handler handler)
{
    dispatch_.unordered(
        &notifier::do_renew,
            shared_from_this(), request, handler);
}

void notifier::do_renew(const incoming& request, send_handler handler)
{
    binary filter;
    subscribe_type type;

    if (!deserialize_address(filter, type, request.data()))
    {
        log::warning(LOG_SERVICE)
            << "Incorrect format for subscribe renew.";
        return;
    }

    const auto expire_time = now() + settings_.subscription_expiration();

    // Find entry and update expiry_time.
    for (auto& subscription: subscriptions_)
    {
        if (subscription.type != type)
            continue;

        // Only update subscriptions which were created by
        // the same client as this request originated from.
        if (subscription.client_origin != request.origin())
            continue;

        // Find matching subscription.
        if (!subscription.prefix.is_prefix_of(filter))
            continue;

        // Future expiry time.
        subscription.expiry_time = expire_time;
    }

    // Send response.
    data_chunk result(sizeof(uint32_t));
    auto serial = make_serializer(result.begin());
    serial.write_error_code(error::success);
    outgoing response(request, result);

    // This is the end of the renew sequence.
    handler(response);
}

// ----------------------------------------------------------------------------
// Scan sequence.

void notifier::scan(uint32_t height, const hash_digest& block_hash,
    const transaction& tx)
{
    dispatch_.ordered(
        &notifier::do_scan,
            shared_from_this(), height, block_hash, tx);
}

void notifier::do_scan(uint32_t height, const hash_digest& block_hash,
    const transaction& tx)
{
    for (const auto& input: tx.inputs)
    {
        const auto address = payment_address::extract(input.script);

        if (address)
            post_updates(address, height, block_hash, tx);
    }

    uint32_t prefix;
    for (const auto& output: tx.outputs)
    {
        const auto address = payment_address::extract(output.script);

        if (address)
            post_updates(address, height, block_hash, tx);
        else if (to_stealth_prefix(prefix, output.script))
            post_stealth_updates(prefix, height, block_hash, tx);
    }

    // This is the end of the scan sequence.
    // Periodicially sweep old expired entries.
    // Use the block 10 minute window as a periodic trigger.
    if (height > 0)
        sweep();
}

void notifier::post_updates(const payment_address& address,
    uint32_t height, const hash_digest& block_hash, const transaction& tx)
{
    // [ address.version:1 ]
    // [ address.hash:20 ]
    // [ height:4 ]
    // [ block_hash:32 ]
    // [ tx ]
    static constexpr size_t info_size = sizeof(uint8_t) + short_hash_size +
        sizeof(uint32_t) + hash_size;

    const auto tx_size64 = tx.serialized_size();
    BITCOIN_ASSERT(tx_size64 <= max_size_t);
    const auto tx_size = static_cast<size_t>(tx_size64);

    data_chunk data(info_size + tx_size);
    auto serial = make_serializer(data.begin());
    serial.write_byte(address.version());
    serial.write_short_hash(address.hash());
    serial.write_4_bytes_little_endian(height);
    serial.write_hash(block_hash);
    BITCOIN_ASSERT(serial.iterator() == data.begin() + info_size);

    // Now write the tx part.
    data_chunk tx_data = tx.to_data();
    serial.write_data(tx_data);
    BITCOIN_ASSERT(serial.iterator() == data.end());

    // Send the result to everyone interested.
    for (const auto& subscription: subscriptions_)
    {
        if (subscription.type != subscribe_type::address)
            continue;

        if (!subscription.prefix.is_prefix_of(address.hash()))
            continue;

        outgoing update("address.update", data, subscription.client_origin);

        subscription.handler(update);
    }
}

void notifier::post_stealth_updates(uint32_t prefix, uint32_t height,
    const hash_digest& block_hash, const transaction& tx)
{
    // [ prefix:4 ]
    // [ height:4 ] 
    // [ block_hash:32 ]
    // [ tx ]
    static constexpr size_t info_size = sizeof(uint32_t) + sizeof(uint32_t) +
        hash_size;

    const auto tx_size64 = tx.serialized_size();
    BITCOIN_ASSERT(tx_size64 <= max_size_t);
    const auto tx_size = static_cast<size_t>(tx_size64);

    data_chunk data(info_size + tx_size);
    auto serial = make_serializer(data.begin());
    serial.write_4_bytes_little_endian(prefix);
    serial.write_4_bytes_little_endian(height);
    serial.write_hash(block_hash);
    BITCOIN_ASSERT(serial.iterator() == data.begin() + info_size);

    // Now write the tx part.
    auto tx_data = tx.to_data();
    serial.write_data(tx_data);
    BITCOIN_ASSERT(serial.iterator() == data.end());

    // Send the result to everyone interested.
    for (const auto& subscription: subscriptions_)
    {
        if (subscription.type != subscribe_type::stealth)
            continue;

        if (!subscription.prefix.is_prefix_of(prefix))
            continue;

        outgoing update("address.stealth_update", data,
            subscription.client_origin);

        subscription.handler(update);
    }
}

code notifier::add(const incoming& request, send_handler handler)
{
    binary address_key;
    subscribe_type type;

    if (!deserialize_address(address_key, type, request.data()))
    {
        log::warning(LOG_SERVICE)
            << "Incorrect format for subscribe data.";
        return error::bad_stream;
    }

    // Limit absolute number of subscriptions to prevent exhaustion attacks.
    if (subscriptions_.size() >= settings_.subscription_limit)
        return error::pool_filled;

    // Now create subscription.
    const auto expire_time = now() + settings_.subscription_expiration();
    const subscription new_subscription =
    {
        address_key,
        expire_time,
        request.origin(),
        handler,
        type
    };

    subscriptions_.emplace_back(new_subscription);
    return error::success;
}

void notifier::sweep()
{
    const auto fixed_time = now();

    // Delete entries that have expired.
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();)
    {
        const auto& subscription = *it;

        // Already expired? If so, then erase.
        if (subscription.expiry_time < fixed_time)
        {
            log::debug(LOG_SERVICE)
                << "Deleting expired subscription: " << subscription.prefix
                << " from " << encode_base16(subscription.client_origin);

            it = subscriptions_.erase(it);
            continue;
        }

        ++it;
    }
}

// ----------------------------------------------------------------------------
// Destruct sequence.

void notifier::stop()
{
    threadpool_.shutdown();
}

void notifier::close()
{
    stop();

    // This is the end of the destruct sequence.
    threadpool_.join();
}

notifier::~notifier()
{
    close();
}

} // namespace server
} // namespace libbitcoin
