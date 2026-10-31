#pragma once

#include <map>
#include <string>
#include <sstream>
#include <lstate.h>
#include <lualib.h>
#include <optional>
#include <algorithm>
#include <Windows.h>
#include <yarafunk.h>
#include <cpr/cpr.h>
#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Security/BrandGuard.hpp>
#include <Dependencies/nlohmann/json.hpp>
#include <Scripting/Coroutines/Yielding.hpp>

#define CPPHTTPLIB_MBEDTLS_SUPPORT
#define CPPHTTPLIB_HEADER_MAX_LENGTH 65536
#include <bcrypt.h>
#include <httplib.h>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace Http
{
    std::string gethwid()
    {
        HW_PROFILE_INFO hw_info{};
        std::string clean;

        if (GetCurrentHwProfile(&hw_info))
        {
            std::wstring w_guid(hw_info.szHwProfileGuid);
            std::string guid(w_guid.begin(), w_guid.end());
            for (char c : guid)
            {
                if (c != '{' && c != '}' && c != '-')
                    clean += c;
            }
        }

        if (clean.empty())
            clean = xorstr("fallback");

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;

        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return clean;

        DWORD hash_length = 0;
        DWORD obj_length = 0;
        DWORD bytes_done = 0;

        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length), &bytes_done, 0);
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&obj_length), sizeof(obj_length), &bytes_done, 0);

        std::vector<uint8_t> hash_obj(obj_length);
        std::vector<uint8_t> digest(hash_length);

        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash,
            hash_obj.data(), obj_length, nullptr, 0, 0)))
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return clean;
        }

        BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(clean.data()),
            static_cast<ULONG>(clean.size()), 0);

        char computer_name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD name_len = sizeof(computer_name);
        if (GetComputerNameA(computer_name, &name_len))
            BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(computer_name),
                name_len, 0);

        DWORD volume_serial = 0;
        if (GetVolumeInformationA(xorstr("C:\\"), nullptr, 0,
            &volume_serial, nullptr, nullptr, nullptr, 0))
            BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(&volume_serial),
                sizeof(volume_serial), 0);

        BCryptFinishHash(hash, digest.data(), hash_length, 0);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);

        std::string result(hash_length * 2, '\0');
        for (size_t i = 0; i < hash_length; i++)
            std::snprintf(&result[i * 2], 3, xorstr("%02x"), digest[i]);

        return result;
    }

    struct game_ids_t
    {
        std::string job_id;
        std::string place_id;
    };
    game_ids_t read_game_ids(lua_State* L)
    {
        game_ids_t ids{};
        lua_getglobal(L, xorstr("game"));
        if (lua_isuserdata(L, -1))
        {
            lua_getfield(L, -1, xorstr("JobId"));
            if (lua_isstring(L, -1))
                ids.job_id = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, xorstr("PlaceId"));
            if (lua_isstring(L, -1))
                ids.place_id = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return ids;
    }

    const std::string& fingerprint_header()
    {
        static const std::string name = BrandGuard::Name() + xorstr("-Fingerprint");
        return name;
    }

    // Luarmor's executor spec: game:HttpGet/HttpPost must look like the engine's own client
    // (User-Agent "Roblox/WinInet"); only request() carries the executor's whitelisted User-Agent.
    void add_identity_headers(lua_State* L, httplib::Headers& headers, bool is_game_http)
    {
        auto has_header = [&](const std::string& key) -> bool
            {
                for (auto& h : headers)
                    if (_stricmp(h.first.c_str(), key.c_str()) == 0)
                        return true;
                return false;
            };
        auto add = [&](const std::string& key, const std::string& value)
            {
                if (!has_header(key))
                    headers.insert({ key, value });
            };

        game_ids_t ids = read_game_ids(L);
        std::string hwid = gethwid();

        nlohmann::json session_id_json;
        session_id_json[xorstr("GameId")] = ids.job_id;
        session_id_json[xorstr("PlaceId")] = ids.place_id;

        add(xorstr("Roblox-Session-Id"), session_id_json.dump());
        add(xorstr("Roblox-Place-Id"), ids.place_id);
        add(xorstr("Roblox-Game-Id"), ids.job_id);
        add(xorstr("Exploit-Guid"), hwid);
        add(xorstr("Accept"), xorstr("*/*"));

        add(OBF("User-Agent"), is_game_http ? std::string(xorstr("Roblox/WinInet")) : BrandGuard::UserAgent());
        add(OBF("Exploit-Identifier"), BrandGuard::UserAgent());
        add(BrandGuard::Name() + xorstr("-User-Identifier"), hwid);
        add(fingerprint_header(), hwid);
    }

    inline std::unordered_set<std::string> blocked_domains = {
    "accountinformation.roblox.com", "accountsettings.roblox.com",
    "twostepverification.roblox.com", "trades.roblox.com",
    "billing.roblox.com", "economy.roblox.com", "auth.roblox.com",
    "accountinformation.roproxy.com", "accountsettings.roproxy.com",
    "twostepverification.roproxy.com", "trades.roproxy.com",
    "billing.roproxy.com", "economy.roproxy.com", "auth.roproxy.com",
    "api.ipify.org", "api4.ipify.org", "api6.ipify.org", "api64.ipify.org",
    "api.ipapi.is", "api.ipapi.co", "api.ipapi.com",
    "api.ipinfo.io", "api.ipgeolocation.io", "api.freegeoip.app",
    "api.myip.com", "api.ip.sb", "api.ipwhois.io",
    "api.ipdata.co", "api.ipstack.com", "api.db-ip.com",
    "api.seeip.org", "api.ipregistry.co", "api.abstractapi.com",
    "api.ipbase.com", "api.iplocation.net", "api.ip.nf",
    "api.bigdatacloud.net", "api.techniknews.net", "api.country.is",
    "api.2ip.me", "api.reallyfreegeoip.org",
    };

    bool is_url_blocked(const std::string& url)
    {
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (const auto& domain : blocked_domains)
            if (lower.find(domain) != std::string::npos)
                return true;
        return false;
    }

    struct http_result_t
    {
        bool success = false;
        int status_code = 0;
        std::string body;
        std::string error;
        httplib::Headers headers;
    };
    enum request_method_t
    {
        H_GET,
        H_HEAD,
        H_POST,
        H_PUT,
        H_DELETE,
        H_OPTIONS,
        H_PATCH
    };
    inline std::map<std::string, request_method_t> request_method_map = {
    { "get",     H_GET     },
    { "head",    H_HEAD    },
    { "post",    H_POST    },
    { "put",     H_PUT     },
    { "delete",  H_DELETE  },
    { "options", H_OPTIONS },
    { "patch",   H_PATCH   }
    };

    std::string RewriteApiDumpUrl(const std::string& url)
    {
        if (url.find(xorstr("setup.roblox.com")) != std::string::npos &&
            url.find(xorstr("API-Dump.json")) != std::string::npos)
        {
            return xorstr("https://raw.githubusercontent.com/MaximumADHD/Roblox-Client-Tracker/refs/heads/roblox/API-Dump.json");
        }
        return url;
    }

    struct parsed_url_t
    {
        std::string scheme;
        std::string host;
        int port = 0;
        std::string path;
    };
    parsed_url_t parse_url(const std::string& url)
    {
        parsed_url_t p{};

        size_t scheme_end = url.find(xorstr("://"));
        if (scheme_end == std::string::npos)
            return p;

        p.scheme = url.substr(0, scheme_end);
        std::transform(p.scheme.begin(), p.scheme.end(), p.scheme.begin(), ::tolower);

        size_t host_start = scheme_end + 3;
        size_t path_start = url.find('/', host_start);
        std::string host_port;

        if (path_start == std::string::npos)
        {
            host_port = url.substr(host_start);
            p.path = xorstr("/");
        }
        else
        {
            host_port = url.substr(host_start, path_start - host_start);
            p.path = url.substr(path_start);
        }

        size_t colon = host_port.find(':');
        if (colon != std::string::npos)
        {
            p.host = host_port.substr(0, colon);
            p.port = std::stoi(host_port.substr(colon + 1));
        }
        else
        {
            p.host = host_port;
            p.port = (p.scheme == xorstr("https")) ? 443 : 80;
        }

        return p;
    }

    std::string resolve_location(const std::string& current, const std::string& location)
    {
        if (location.find(xorstr("://")) != std::string::npos)
            return location;

        parsed_url_t p = parse_url(current);
        std::string origin = p.scheme + xorstr("://") + p.host;
        if (!((p.scheme == xorstr("https") && p.port == 443) || (p.scheme == xorstr("http") && p.port == 80)))
            origin += xorstr(":") + std::to_string(p.port);

        if (location.rfind(xorstr("//"), 0) == 0)
            return p.scheme + xorstr(":") + location;
        if (!location.empty() && location[0] == '/')
            return origin + location;

        std::string base = p.path.substr(0, p.path.find('?'));
        base = base.substr(0, base.rfind('/') + 1);
        return origin + base + location;
    }

    http_result_t do_single_request(
        request_method_t method, const std::string& url,
        const httplib::Headers& headers, const std::string& body, const std::string& content_type);

    http_result_t do_request(
        request_method_t method, const std::string& url,
        const httplib::Headers& headers, const std::string& body, const std::string& content_type)
    {
        std::string current = url;
        request_method_t current_method = method;
        std::string current_body = body;

        for (int hop = 0; hop <= 10; ++hop)
        {
            http_result_t result;
            try
            {
                result = do_single_request(current_method, current, headers, current_body, content_type);
            }
            catch (const std::exception& ex)
            {
                result = {};
                result.error = std::string(xorstr("request failed: ")) + ex.what();
            }
            if (!result.success)
                return result;

            const int code = result.status_code;
            if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
                return result;

            auto location = result.headers.find(xorstr("Location"));
            if (location == result.headers.end() || location->second.empty())
                return result;

            if (code == 303 || ((code == 301 || code == 302) && current_method == H_POST))
            {
                current_method = H_GET;
                current_body.clear();
            }
            current = resolve_location(current, location->second);
        }

        http_result_t result{};
        result.error = xorstr("request failed: too many redirects");
        return result;
    }

    http_result_t do_single_request(
        request_method_t method, const std::string& url,
        const httplib::Headers& headers, const std::string& body, const std::string& content_type)
    {
        http_result_t result{};

        parsed_url_t p = parse_url(url);
        if (p.host.empty())
        {
            result.error = xorstr("invalid url");
            return result;
        }

        std::string scheme_host;
        bool is_default_port = (p.scheme == xorstr("https") && p.port == 443) || (p.scheme == xorstr("http") && p.port == 80);
        if (is_default_port)
            scheme_host = p.scheme + xorstr("://") + p.host;
        else
            scheme_host = p.scheme + xorstr("://") + p.host + xorstr(":") + std::to_string(p.port);
        std::string ct = content_type.empty() ? xorstr("application/octet-stream") : content_type;

        try
        {
            httplib::Client cli(scheme_host);
            cli.set_follow_location(false);
            cli.set_connection_timeout(30, 0);
            cli.set_read_timeout(30, 0);
            cli.set_write_timeout(30, 0);

            if (p.scheme == xorstr("https"))
                cli.enable_server_certificate_verification(false);

            httplib::Result res;

            switch (method)
            {
            case H_GET:     res = cli.Get(p.path, headers); break;
            case H_HEAD:    res = cli.Head(p.path, headers); break;
            case H_POST:    res = cli.Post(p.path, headers, body, ct.c_str()); break;
            case H_PUT:     res = cli.Put(p.path, headers, body, ct.c_str()); break;
            case H_DELETE:  res = cli.Delete(p.path, headers, body, ct.c_str()); break;
            case H_OPTIONS: res = cli.Options(p.path, headers); break;
            case H_PATCH:   res = cli.Patch(p.path, headers, body, ct.c_str()); break;
            default:
                result.error = xorstr("invalid request method");
                return result;
            }

            if (!res)
            {
                result.error = xorstr("request failed: ") + httplib::to_string(res.error());
                return result;
            }

            result.success = true;
            result.status_code = res->status;
            result.body = res->body;
            result.headers = res->headers;
        }
        catch (const std::exception& ex)
        {
            result.error = std::string(xorstr("request failed: ")) + ex.what();
        }
        catch (...)
        {
            result.error = xorstr("request failed: unknown exception");
        }

        return result;
    }
    int HttpGet(lua_State* L)
    {
        std::string url;

        if (!lua_isstring(L, 1))
        {
            luaL_checkstring(L, 2);
            url = lua_tostring(L, 2);
        }
        else
        {
            url = lua_tostring(L, 1);
        }

        if (url.find(xorstr("http://")) != 0 && url.find(xorstr("https://")) != 0)
        {
            luaL_argerror(L, 2, xorstr("invalid protocol (expected 'http' or 'https')"));
            return 0;
        }

        if (is_url_blocked(url))
        {
            luaL_error(L, xorstr("url is blocked"));
            return 0;
        }

        url = RewriteApiDumpUrl(url);

        httplib::Headers headers;
        add_identity_headers(L, headers, true);

        return Yielding::YieldExecution(L, [url, headers]() -> std::function<int(lua_State*)>
            {
                http_result_t result = do_request(
                    H_GET,
                    url,
                    headers,
                    "",
                    ""
                );

                return [result](lua_State* L) -> int
                    {
                        if (!result.success)
                        {
                            lua_pushnil(L);
                            return 1;
                        }

                        if (result.status_code < 200 || result.status_code >= 300)
                        {
                            lua_pushnil(L);
                            return 1;
                        }

                        lua_pushlstring(L, result.body.data(), result.body.size());
                        return 1;
                    };
            });
    }

    int HttpPost(lua_State* L)
    {
        std::string url;
        int arg = 1;

        if (!lua_isstring(L, 1))
        {
            luaL_checkstring(L, 2);
            url = lua_tostring(L, 2);
            arg = 3;
        }
        else
        {
            url = lua_tostring(L, 1);
            arg = 2;
        }

        if (url.find(xorstr("http://")) != 0 && url.find(xorstr("https://")) != 0)
        {
            luaL_argerror(L, 2, xorstr("invalid protocol (expected 'http' or 'https')"));
            return 0;
        }

        if (is_url_blocked(url))
        {
            luaL_error(L, xorstr("url is blocked"));
            return 0;
        }

        url = RewriteApiDumpUrl(url);

        std::string data = "";
        if (lua_isstring(L, arg))
        {
            data = lua_tostring(L, arg);
            arg++;
        }

        std::string content_type = "application/json";
        if (lua_isstring(L, arg))
        {
            content_type = lua_tostring(L, arg);
            arg++;
        }

        httplib::Headers headers;
        add_identity_headers(L, headers, true);

        return Yielding::YieldExecution(L, [url, headers, data, content_type]() -> std::function<int(lua_State*)>
            {
                http_result_t result = do_request(
                    H_POST,
                    url,
                    headers,
                    data,
                    content_type
                );

                return [result](lua_State* L) -> int
                    {
                        if (!result.success)
                        {
                            lua_pushnil(L);
                            return 1;
                        }

                        if (result.status_code < 200 || result.status_code >= 300)
                        {
                            lua_pushnil(L);
                            return 1;
                        }

                        lua_pushlstring(L, result.body.data(), result.body.size());
                        return 1;
                    };
            });
    }

    std::string get_status_phrase(int code)
    {
        switch (code)
        {
        case 100: return xorstr("Continue");
        case 101: return xorstr("Switching Protocols");
        case 102: return xorstr("Processing");
        case 103: return xorstr("Early Hints");
        case 200: return xorstr("OK");
        case 201: return xorstr("Created");
        case 202: return xorstr("Accepted");
        case 203: return xorstr("Non-Authoritative Information");
        case 204: return xorstr("No Content");
        case 205: return xorstr("Reset Content");
        case 206: return xorstr("Partial Content");
        case 207: return xorstr("Multi-Status");
        case 208: return xorstr("Already Reported");
        case 226: return xorstr("IM Used");
        case 300: return xorstr("Multiple Choices");
        case 301: return xorstr("Moved Permanently");
        case 302: return xorstr("Found");
        case 303: return xorstr("See Other");
        case 304: return xorstr("Not Modified");
        case 305: return xorstr("Use Proxy");
        case 307: return xorstr("Temporary Redirect");
        case 308: return xorstr("Permanent Redirect");
        case 400: return xorstr("Bad Request");
        case 401: return xorstr("Unauthorized");
        case 402: return xorstr("Payment Required");
        case 403: return xorstr("Forbidden");
        case 404: return xorstr("Not Found");
        case 405: return xorstr("Method Not Allowed");
        case 406: return xorstr("Not Acceptable");
        case 407: return xorstr("Proxy Authentication Required");
        case 408: return xorstr("Request Timeout");
        case 409: return xorstr("Conflict");
        case 410: return xorstr("Gone");
        case 411: return xorstr("Length Required");
        case 412: return xorstr("Precondition Failed");
        case 413: return xorstr("Payload Too Large");
        case 414: return xorstr("URI Too Long");
        case 415: return xorstr("Unsupported Media Type");
        case 416: return xorstr("Range Not Satisfiable");
        case 417: return xorstr("Expectation Failed");
        case 418: return xorstr("I'm a teapot");
        case 422: return xorstr("Unprocessable Entity");
        case 423: return xorstr("Locked");
        case 424: return xorstr("Failed Dependency");
        case 426: return xorstr("Upgrade Required");
        case 428: return xorstr("Precondition Required");
        case 429: return xorstr("Too Many Requests");
        case 431: return xorstr("Request Header Fields Too Large");
        case 451: return xorstr("Unavailable For Legal Reasons");
        case 500: return xorstr("Internal Server Error");
        case 501: return xorstr("Not Implemented");
        case 502: return xorstr("Bad Gateway");
        case 503: return xorstr("Service Unavailable");
        case 504: return xorstr("Gateway Time-out");
        case 505: return xorstr("HTTP Version Not Supported");
        case 506: return xorstr("Variant Also Negotiates");
        case 507: return xorstr("Insufficient Storage");
        case 508: return xorstr("Loop Detected");
        case 510: return xorstr("Not Extended");
        case 511: return xorstr("Network Authentication Required");
        default:  return std::string();
        }
    }
    int request(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);

        lua_getfield(L, 1, xorstr("Url"));
        if (!lua_isstring(L, -1))
            luaL_error(L, xorstr("missing or invalid 'Url'"));

        std::string url = lua_tostring(L, -1);
        lua_pop(L, 1);

        if (url.find(xorstr("http://")) != 0 && url.find(xorstr("https://")) != 0)
        {
            luaL_error(L, xorstr("invalid protocol (expected 'http' or 'https')"));
            return 0;
        }

        if (is_url_blocked(url))
        {
            luaL_error(L, xorstr("url is blocked"));
            return 0;
        }

        url = RewriteApiDumpUrl(url);

        auto method = H_GET;
        lua_getfield(L, 1, xorstr("Method"));
        if (lua_isstring(L, -1))
        {
            std::string method_str = lua_tostring(L, -1);
            std::transform(method_str.begin(), method_str.end(), method_str.begin(), ::tolower);
            if (!request_method_map.count(method_str))
                luaL_error(L, xorstr("invalid request method: '%s'"), method_str.c_str());

            method = request_method_map[method_str];
        }
        lua_pop(L, 1);

        httplib::Headers headers;
        lua_getfield(L, 1, xorstr("Headers"));
        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2))
            {
                if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
                    luaL_error(L, xorstr("'Headers' must have string keys/values"));

                std::string key = lua_tostring(L, -2);
                if (_stricmp(key.c_str(), xorstr("Content-Length")) == 0)
                    luaL_error(L, xorstr("'Content-Length' cannot be overwritten"));

                headers.insert({ key, lua_tostring(L, -1) });
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        std::string cookie_header;
        lua_getfield(L, 1, xorstr("Cookies"));
        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2))
            {
                if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
                    luaL_error(L, xorstr("'Cookies' must have string keys/values"));

                if (!cookie_header.empty()) cookie_header += xorstr("; ");
                cookie_header += lua_tostring(L, -2);
                cookie_header += xorstr("=");
                cookie_header += lua_tostring(L, -1);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        if (!cookie_header.empty())
            headers.insert({ xorstr("Cookie"), cookie_header });

        std::string body;
        lua_getfield(L, 1, xorstr("Body"));
        if (lua_isstring(L, -1))
        {
            if (method == H_GET || method == H_HEAD)
                luaL_error(L, xorstr("'Body' is not allowed for GET/HEAD requests"));

            size_t body_length = 0;
            const char* body_data = lua_tolstring(L, -1, &body_length);
            body.assign(body_data, body_length);
        }
        lua_pop(L, 1);

        add_identity_headers(L, headers, false);

        return Yielding::YieldExecution(L, [=]() -> std::function<int(lua_State*)>
            {
                http_result_t response = do_request(
                    method,
                    url,
                    headers,
                    body,
                    ""
                );

                return [response](lua_State* L) -> int
                    {
                        if (!response.success)
                        {
                            lua_newtable(L);

                            lua_pushboolean(L, false);
                            lua_setfield(L, -2, xorstr("Success"));

                            lua_pushinteger(L, 0);
                            lua_setfield(L, -2, xorstr("StatusCode"));

                            lua_pushstring(L, response.error.c_str());
                            lua_setfield(L, -2, xorstr("StatusMessage"));

                            lua_newtable(L);
                            lua_setfield(L, -2, xorstr("Headers"));

                            lua_newtable(L);
                            lua_setfield(L, -2, xorstr("Cookies"));

                            lua_pushlstring(L, "", 0);
                            lua_setfield(L, -2, xorstr("Body"));

                            return 1;
                        }

                        lua_newtable(L);
                        lua_pushboolean(L, response.status_code >= 200 && response.status_code < 300);
                        lua_setfield(L, -2, xorstr("Success"));

                        lua_pushinteger(L, response.status_code);
                        lua_setfield(L, -2, xorstr("StatusCode"));

                        std::string phrase = get_status_phrase(response.status_code);
                        lua_pushstring(L, phrase.c_str());
                        lua_setfield(L, -2, xorstr("StatusMessage"));

                        lua_newtable(L);
                        for (auto& header : response.headers)
                        {
                            lua_pushstring(L, header.first.c_str());
                            lua_pushstring(L, header.second.c_str());
                            lua_settable(L, -3);
                        }
                        lua_setfield(L, -2, xorstr("Headers"));

                        lua_newtable(L);
                        auto set_cookie_range = response.headers.equal_range(xorstr("Set-Cookie"));
                        for (auto it = set_cookie_range.first; it != set_cookie_range.second; ++it)
                        {
                            const std::string& cookie = it->second;
                            size_t eq = cookie.find('=');
                            size_t sc = cookie.find(';');
                            if (eq == std::string::npos) continue;
                            std::string key = cookie.substr(0, eq);
                            std::string val = cookie.substr(eq + 1, (sc == std::string::npos ? cookie.size() : sc) - eq - 1);
                            lua_pushstring(L, key.c_str());
                            lua_pushstring(L, val.c_str());
                            lua_settable(L, -3);
                        }
                        lua_setfield(L, -2, xorstr("Cookies"));

                        lua_pushlstring(L, response.body.data(), response.body.size());
                        lua_setfield(L, -2, xorstr("Body"));

                        return 1;
                    };
            });
    }

    void RegisterLibrary(lua_State* L)
    {

        Utils::AddFunction(L, "HttpGet", Http::HttpGet);
        Utils::AddFunction(L, "HttpPost", Http::HttpPost);
        Utils::AddFunction(L, "request", Http::request);
        Utils::AddFunction(L, "http_request", Http::request);

        lua_newtable(L);
        Utils::AddTableFunction(L, "request", Http::request);
        lua_setglobal(L, "http");
    }
}
