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
#ifndef LIBBITCOIN_SERVER_CONFIGURATION_HPP
#define LIBBITCOIN_SERVER_CONFIGURATION_HPP

#include <bitcoin/node.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/settings.hpp>

namespace libbitcoin {
namespace server {

// Log names.
#define LOG_REQUEST "request"
#define LOG_SERVICE "service"

// Not localizable.
#define BS_HELP_VARIABLE "help"
#define BS_SETTINGS_VARIABLE "settings"
#define BS_VERSION_VARIABLE "version"

// This must be lower case but the env var part can be any case.
#define BS_CONFIG_VARIABLE "config"

// This must match the case of the env var.
#define BS_ENVIRONMENT_VARIABLE_PREFIX "BS_"

/// Full server node configuration, thread safe.
class BCS_API configuration
  : public node::configuration
{
public:
    /// Default instances.
    static const configuration mainnet;
    static const configuration testnet;

    configuration();
    configuration(
        const server::settings& server_settings,
        const node::settings& node_settings,
        const blockchain::settings& chain_settings,
        const database::settings& database_settings,
        const network::settings& network_settings);

    /// Settings.
    server::settings server;
};

} // namespace server
} // namespace libbitcoin

#endif
