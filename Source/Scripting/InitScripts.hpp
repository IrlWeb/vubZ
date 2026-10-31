#pragma once

#include <string>
#include "Scripting/Obfuscation.hpp"

namespace LuaScripts
{

    std::string signal = R"LUA(local Signal = {}
Signal.__index = Signal

function Signal.new()
    local self = setmetatable({}, Signal)
    self._connections = {}
    return self
end

function Signal:Connect(callback)
    local conn = {
        _callback = callback,
        _signal = self,
        _connected = true,
    }
    conn.Disconnect = function(c)
        c._connected = false
        for i, v in ipairs(self._connections) do
            if v == c then
                table.remove(self._connections, i)
                break
            end
        end
    end
    table.insert(self._connections, conn)
    return conn
end

function Signal:Once(callback)
    local conn
    conn = self:Connect(function(...)
        conn:Disconnect()
        callback(...)
    end)
    return conn
end

function Signal:Fire(...)
    for _, conn in ipairs(table.clone(self._connections)) do
        if conn._connected then
            task.spawn(conn._callback, ...)
        end
    end
end

function Signal:Wait()
    local thread = coroutine.running()
    local conn
    conn = self:Connect(function(...)
        conn:Disconnect()
        task.spawn(thread, ...)
    end)
    return coroutine.yield()
end

getgenv().Signal = Signal)LUA";

    std::string FilterGc = SUPQ_OBF(R"(getgenv().filtergc = newcclosure(function(...)
    local args = {...}
    local type_or_callback = args[1]

    local function valuesEqual(a, b)
        if a == b then
            return true
        end
        return typeof(a) == "number" and typeof(b) == "number" and a ~= a and b ~= b
    end

    local function safeFind(t, target)
        for i, v in t do
            if valuesEqual(v, target) then
                return i
            end
        end
        return nil
    end

    if typeof(type_or_callback) == "function" then
        local callback = type_or_callback
        local matches = {}

        for i, v in getgc(true) do
            local success, passed = pcall(callback, v)
            if success and passed then
                table.insert(matches, v)
            end
        end

        return matches
    end

    local filterType = type_or_callback
    local filterOptions = args[2] or {}
    local returnOne = args[3]
    local matches = {}

    if filterType == "table" then
        for i, v in getgc(true) do
            if typeof(v) ~= "table" then
                continue
            end

            local passed = true

            if filterOptions.Keys and typeof(filterOptions.Keys) == "table" and passed then
                for _, key in filterOptions.Keys do
                    if rawget(v, key) == nil then
                        passed = false
                        break
                    end
                end
            end

            if filterOptions.Values and typeof(filterOptions.Values) == "table" and passed then
                local tableVals = {}
                for _, value in next, v do
                    table.insert(tableVals, value)
                end
                for _, value in filterOptions.Values do
                    if not safeFind(tableVals, value) then
                        passed = false
                        break
                    end
                end
            end

            if filterOptions.KeyValuePairs and typeof(filterOptions.KeyValuePairs) == "table" and passed then
                for key, value in filterOptions.KeyValuePairs do
                    if not valuesEqual(rawget(v, key), value) then
                        passed = false
                        break
                    end
                end
            end

            if filterOptions.Metatable and passed then
                local success, mt = pcall(getrawmetatable, v)
                if success then
                    passed = filterOptions.Metatable == mt
                else
                    passed = false
                end
            end

            if passed then
                if returnOne then
                    return v
                else
                    table.insert(matches, v)
                end
            end
        end

    elseif filterType == "function" then
        if filterOptions.IgnoreExecutor == nil then
            filterOptions.IgnoreExecutor = true
        end

        for i, v in getgc(false) do
            if typeof(v) ~= "function" then
                continue
            end

            local passed = true
            local isCClosure = pcall(function() return iscclosure(v) end) and iscclosure(v) or false

            if filterOptions.Name and passed then
                local success, funcName = pcall(function()
                    local info = debug.info(v, "n")
                    return info
                end)
                if success and funcName then
                    passed = funcName == filterOptions.Name
                else
                    local success2, funcString = pcall(function()
                        return tostring(v)
                    end)
                    if success2 and funcString then
                        passed = string.find(funcString, filterOptions.Name) ~= nil
                    else
                        passed = false
                    end
                end
            end

            if filterOptions.IgnoreExecutor == true and passed then
                local success, isExec = pcall(function() return isexecutorclosure(v) end)
                if success then
                    passed = not isExec
                else
                    passed = true
                end
            end

            if isCClosure and (filterOptions.Hash or filterOptions.Constants or filterOptions.Upvalues) then
                passed = false
            end

            if not isCClosure and passed then
                if filterOptions.Hash and passed then
                    local success, hash = pcall(function()
                        return getfunctionhash and getfunctionhash(v) or nil
                    end)
                    if success and hash then
                        passed = hash == filterOptions.Hash
                    else
                        passed = false
                    end
                end

                if filterOptions.Constants and typeof(filterOptions.Constants) == "table" and passed then
                    local success, constants = pcall(function()
                        return debug.getconstants and debug.getconstants(v) or {}
                    end)
                    if success and constants then
                        local funcConsts = {}
                        for idx, constant in constants do
                            if constant ~= nil then
                                table.insert(funcConsts, constant)
                            end
                        end
                        for _, constant in filterOptions.Constants do
                            if not safeFind(funcConsts, constant) then
                                passed = false
                                break
                            end
                        end
                    else
                        passed = false
                    end
                end

                if filterOptions.Upvalues and typeof(filterOptions.Upvalues) == "table" and passed then
                    local success, upvalues = pcall(function()
                        return debug.getupvalues and debug.getupvalues(v) or {}
                    end)
                    if success and upvalues then
                        local funcUpvals = {}
                        for idx, upval in upvalues do
                            if upval ~= nil then
                                table.insert(funcUpvals, upval)
                            end
                        end
                        for _, upval in filterOptions.Upvalues do
                            if not safeFind(funcUpvals, upval) then
                                passed = false
                                break
                            end
                        end
                    else
                        passed = false
                    end
                end
            end

            if passed then
                if returnOne then
                    return v
                else
                    table.insert(matches, v)
                end
            end
        end

    else
        error("Expected type 'function' or 'table', got '" .. tostring(filterType) .. "'")
    end

    return returnOne and nil or matches
end, 'filtergc'))");

    std::string Environment = SUPQ_OBF(R"LUA(local loadfile = function(path, chunkname)
    local success, content = pcall(readfile, path)
    if not success or not content then
        return nil, "cannot open " .. tostring(path) .. ": No such file or directory"
    end

    local chunk_name = chunkname or ("@" .. path)
    return loadstring(content, chunk_name)
end

local dofile = function(path)
    local func, err = loadfile(path)
    if not func then
        error(err, 2)
    end
    return func()
end

getgenv().loadfile = loadfile
getgenv().load_file = loadfile
getgenv().loadfromfile = loadfile
getgenv().load_from_file = loadfile

getgenv().dofile = dofile
getgenv().do_file = dofile
getgenv().runfile = dofile

if not getgenv().file then
    getgenv().file = {}
end

getgenv().file.load = loadfile
getgenv().file.loadfile = loadfile
getgenv().file.load_file = loadfile
getgenv().file["do"] = dofile
getgenv().file.dofile = dofile
getgenv().file.do_file = dofile

if not getgenv().folder then
    getgenv().folder = {}
end

if not getgenv().folder then
    getgenv().folder = {}
end

local game = game
local Instance = Instance

local http = {}
http.request = request or http_request or syn_request

getgenv().http = http
getgenv().syn = getgenv().syn or {}
getgenv().syn.request = http.request

local Signal = {}
Signal.__index = Signal

function Signal.new()
    local self = setmetatable({}, Signal)
    self._connections = {}
    return self
end

function Signal:Connect(callback)
    local connection = {
        Connected = true,
        Callback = callback,
        Disconnect = function(self)
            self.Connected = false
        end
    }
    table.insert(self._connections, connection)
    return connection
end

function Signal:Fire(...)
    for _, connection in ipairs(self._connections) do
        if connection.Connected then
            task.spawn(connection.Callback, ...)
        end
    end
end

function Signal:Wait()
    local thread = coroutine.running()
    local connection
    connection = self:Connect(function(...)
        connection:Disconnect()
        task.spawn(thread, ...)
    end)
    return coroutine.yield()
end

local bit32 = bit32 or bit or {}

if not bit32.band then
    bit32.band = function(a, b)
        local result = 0
        local bitval = 1
        while a > 0 and b > 0 do
            if a % 2 == 1 and b % 2 == 1 then
                result = result + bitval
            end
            bitval = bitval * 2
            a = math.floor(a / 2)
            b = math.floor(b / 2)
        end
        return result
    end
end

if not bit32.bor then
    bit32.bor = function(a, b)
        local result = 0
        local bitval = 1
        while a > 0 or b > 0 do
            if a % 2 == 1 or b % 2 == 1 then
                result = result + bitval
            end
            bitval = bitval * 2
            a = math.floor(a / 2)
            b = math.floor(b / 2)
        end
        return result
    end
end

if not bit32.bxor then
    bit32.bxor = function(a, b)
        local result = 0
        local bitval = 1
        while a > 0 or b > 0 do
            if (a % 2) ~= (b % 2) then
                result = result + bitval
            end
            bitval = bitval * 2
            a = math.floor(a / 2)
            b = math.floor(b / 2)
        end
        return result
    end
end

if not bit32.bnot then
    bit32.bnot = function(n)
        return (-1) - n
    end
end

if not bit32.lshift then
    bit32.lshift = function(a, b)
        return a * (2 ^ b)
    end
end

if not bit32.rshift then
    bit32.rshift = function(a, b)
        return math.floor(a / (2 ^ b))
    end
end

if not bit32.arshift then
    bit32.arshift = function(a, b)
        local result = math.floor(a / (2 ^ b))
        if a < 0 and b > 0 then
            result = result + (2 ^ (32 - b)) - 1
        end
        return result
    end
end

if not bit32.lrotate then
    bit32.lrotate = function(a, b)
        b = b % 32
        return bit32.bor(bit32.lshift(a, b), bit32.rshift(a, 32 - b))
    end
end

if not bit32.rrotate then
    bit32.rrotate = function(a, b)
        b = b % 32
        return bit32.bor(bit32.rshift(a, b), bit32.lshift(a, 32 - b))
    end
end

if not bit32.extract then
    bit32.extract = function(n, field, width)
        width = width or 1
        return bit32.band(bit32.rshift(n, field), (2 ^ width) - 1)
    end
end

if not bit32.replace then
    bit32.replace = function(n, v, field, width)
        width = width or 1
        local mask = (2 ^ width) - 1
        return bit32.bor(
            bit32.band(n, bit32.bnot(bit32.lshift(mask, field))),
            bit32.lshift(bit32.band(v, mask), field)
        )
    end
end

getgenv().bit32 = bit32
getgenv().bit = (type(bit) == "table" and bit.badd) and bit or bit32)LUA");

    std::string Decompile = SUPQ_OBF(R"LUA(local DECOMPILER_URL = "http://127.0.0.1:4128/luau/decompile"

local function base64(data)
	local encoder = (crypt and crypt.base64encode) or base64_encode
	return encoder(data)
end

-- DecompilerOptions / DecompilerFormatter (supQ): userdata that only accept
-- known properties of the right type. Objects are remembered in a weak set,
-- since typeof() reports newproxy userdata as plain "userdata".
local instances = {}

local function isA(value, typeName)
	return instances[typeName] ~= nil and instances[typeName][value] == true
end

local function makeClass(typeName, fields, defaults)
	local class = {}
	local created = setmetatable({}, { __mode = "k" })
	instances[typeName] = created
	function class.new()
		local values = table.clone(defaults)
		for key, value in values do
			if type(value) == "function" then
				values[key] = value()
			end
		end
		local object = newproxy(true)
		local meta = getmetatable(object)
		meta.__type = typeName
		meta.__index = function(_, key)
			if fields[key] == nil then
				error(`{key} is not a valid member of {typeName}`, 2)
			end
			return values[key]
		end
		meta.__newindex = function(_, key, value)
			local expected = fields[key]
			if expected == nil then
				error(`{key} is not a valid member of {typeName}`, 2)
			end
			local valid = if expected == "number" or expected == "boolean"
				then type(value) == expected
				else isA(value, expected)
			if not valid then
				error(`{typeName}.{key} expects {expected}, got {typeof(value)}`, 2)
			end
			values[key] = value
		end
		meta.__tostring = function()
			return typeName
		end
		meta.__metatable = "The metatable is locked"
		created[object] = true
		return object
	end
	return class
end

local Formatter = makeClass("DecompilerFormatter", {
	IndentWidth = "number",
	ColumnLimit = "number",
	ParenthesizeConditions = "boolean",
	AppendSemicolons = "boolean",
	FunctionMetadataEnabled = "boolean",
	FunctionMetadataLayout = "number",
	FunctionMetadataIncludeName = "boolean",
	FunctionMetadataIncludeLine = "boolean",
	FunctionMetadataIncludeUpvalues = "boolean",
	FunctionMetadataUpvalueFormat = "number",
}, {
	IndentWidth = 0, -- 0 indents with tabs
	ColumnLimit = 0,
	ParenthesizeConditions = false,
	AppendSemicolons = false,
	FunctionMetadataEnabled = true,
	FunctionMetadataLayout = 0,
	FunctionMetadataIncludeName = true,
	FunctionMetadataIncludeLine = false,
	FunctionMetadataIncludeUpvalues = true,
	FunctionMetadataUpvalueFormat = 1,
})
Formatter.FunctionMetadata = table.freeze({
	Layout = table.freeze({ Block = 0, Inline = 1 }),
	UpvalueFormat = table.freeze({ Name = 0, Kind = 1, KindAndIndex = 2 }),
})

local Options = makeClass("DecompilerOptions", {
	SmartVariableRenamer = "boolean",
	FunctionDeclarations = "boolean",
	GuardClauses = "boolean",
	ConstantFolding = "boolean",
	ConditionalStructurer = "boolean",
	DoBlockInsertionThreshold = "number",
	Formatter = "DecompilerFormatter",
}, {
	SmartVariableRenamer = true,
	FunctionDeclarations = true,
	GuardClauses = true,
	ConstantFolding = true,
	ConditionalStructurer = true,
	DoBlockInsertionThreshold = 0,
	Formatter = Formatter.new,
})

getgenv().DecompilerFormatter = table.freeze(Formatter)
getgenv().DecompilerOptions = table.freeze(Options)

local QUERY = {
	SmartVariableRenamer = "smart_variable_renamer",
	FunctionDeclarations = "function_declarations",
	GuardClauses = "guard_clauses",
	ConstantFolding = "constant_folding",
	ConditionalStructurer = "conditional_structurer",
	DoBlockInsertionThreshold = "do_block_insertion_threshold",
}
local FORMATTER_QUERY = {
	IndentWidth = "indent_width",
	ColumnLimit = "column_limit",
	ParenthesizeConditions = "parenthesize_conditions",
	AppendSemicolons = "append_semicolons",
	FunctionMetadataEnabled = "function_metadata_enabled",
	FunctionMetadataLayout = "function_metadata_layout",
	FunctionMetadataIncludeName = "function_metadata_include_name",
	FunctionMetadataIncludeLine = "function_metadata_include_line",
	FunctionMetadataIncludeUpvalues = "function_metadata_include_upvalues",
	FunctionMetadataUpvalueFormat = "function_metadata_upvalue_format",
}

local function queryFor(options)
	if options == nil then
		return ""
	end
	if not isA(options, "DecompilerOptions") then
		error("decompile: options must come from DecompilerOptions.new()", 3)
	end
	local parts = {}
	local function add(name, value)
		parts[#parts + 1] = `{name}={type(value) == "number" and math.floor(value) or tostring(value)}`
	end
	for property, name in QUERY do
		add(name, options[property])
	end
	local formatter = options.Formatter
	for property, name in FORMATTER_QUERY do
		add(name, formatter[property])
	end
	return "?" .. table.concat(parts, "&")
end

local LOGO = [==[
--[[
 *                  ____
 *   ___ __ _____  / __ \
 *  (_-</ // / _ \/ /_/ /
 * /___/\_,_/ .__/\___\_\
 *         /_/
 *]==]

local FAILURES = {
	"-- unsupported Luau",
	"-- failed to parse bytecode",
	"-- the decompiler panicked",
	"-- no bytecode was sent",
}

-- supQ: decompile(source: LuaSourceContainer | string, options: DecompilerOptions?) -> string.
-- Yields until done; raises when the input cannot be decompiled.
getgenv().decompile = function(source, options)
	local bytecode, path
	if typeof(source) == "Instance" then
		if not source:IsA("LuaSourceContainer") then
			error("decompile: expected a script, got " .. source.ClassName, 2)
		end
		if source.ClassName == "Script" and source.RunContext ~= Enum.RunContext.Client then
			error("decompile: server Scripts are not supported", 2)
		end
		local ok, result = pcall(getscriptbytecode, source, true)
		if not ok or type(result) ~= "string" or #result == 0 then
			error("decompile: could not read the script's bytecode", 2)
		end
		bytecode = result
		local okName, full = pcall(function() return source:GetFullName() end)
		path = okName and full or source.Name
	elseif type(source) == "string" then
		if #source == 0 then
			error("decompile: bytecode is empty", 2)
		end
		bytecode = source
	else
		error("decompile: expected LuaSourceContainer or bytecode string, got " .. typeof(source), 2)
	end

	-- Tovek names the module table and locals after the script.
	local headers = { ["content-type"] = "text/plain" }
	if typeof(source) == "Instance" then
		local name = source.Name:gsub("[^%w_]", ""):sub(1, 64)
		if #name > 0 then
			headers["x-script-name"] = name
		end
	end

	local started = os.clock()
	local ok, response = pcall(request, {
		Url = DECOMPILER_URL .. queryFor(options),
		Method = "POST",
		Headers = headers,
		Body = base64(bytecode),
	})
	if not ok or not response or response.StatusCode ~= 200 then
		error("decompile: the supQ decompiler is not reachable", 2)
	end

	local body = response.Body or ""
	if #body == 0 then
		error("decompile: the decompiler returned nothing", 2)
	end
	for _, prefix in FAILURES do
		if body:sub(1, #prefix) == prefix then
			error("decompile: " .. (body:match("^%-%-%s*([^\n]*)") or body), 2)
		end
	end

	if not path then
		return body
	end
	local ms = math.floor((os.clock() - started) * 1000 + 0.5)
	return LOGO .. "\n *  Script Path: " .. path .. "\n * Decompiled in: " .. ms .. "ms\n]]\n\n" .. body
end)LUA");

    std::string SaveInstance = SUPQ_OBF(R"LUA(local USSI_URL = "https://raw.githubusercontent.com/luau/SynSaveInstance/main/saveinstance.luau"
local BATCH_URL = "http://127.0.0.1:4128/luau/decompile/batch"
local BATCH_SCRIPTS = 200
local BATCH_BYTES = 4 * 1024 * 1024
local BATCHES_IN_FLIGHT = 4
local MESH_WORKERS = 8

local LOGO = [==[
--[[
 *                  ____
 *   ___ __ _____  / __ \
 *  (_-</ // / _ \/ /_/ /
 * /___/\_,_/ .__/\___\_\
 *         /_/
 *]==]

local genv = getgenv()
local realWarn = warn
-- Captured now so a script overwriting the global cannot hijack it.
local createToast = create_toast

-- SaveInstanceOptions: userdata that only accepts the documented properties.
local OPTION_TYPES = {
	FilePath = "string",
	IgnoreArchivable = "boolean",
	SavePlayerCharacters = "boolean",
	SavePlayers = "boolean",
	DisableCompression = "boolean",
	DecompileScripts = "boolean",
	SaveNonCreatable = "boolean",
	SaveNilInstances = "boolean",
	CopyToClipboard = "boolean",
	IgnoreList = "table",
	DecompilerOptions = "userdata",
	ShowStatus = "boolean",
	SafeMode = "boolean",
	SaveMeshes = "boolean",
}

local optionValues = setmetatable({}, { __mode = "k" })

local function newDecompilerOptions()
	local class = genv.DecompilerOptions
	return class and class.new() or nil
end

local SaveInstanceOptions = {}

function SaveInstanceOptions.new()
	local object = newproxy(true)
	local values = {
		FilePath = "",
		IgnoreArchivable = false,
		SavePlayerCharacters = false,
		SavePlayers = false,
		DisableCompression = false,
		DecompileScripts = true,
		SaveNonCreatable = false,
		SaveNilInstances = false,
		CopyToClipboard = false,
		IgnoreList = {},
		DecompilerOptions = false, -- created on first read
		ShowStatus = true,
		SafeMode = false,
		SaveMeshes = true,
	}
	optionValues[object] = values

	local meta = getmetatable(object)
	meta.__type = "SaveInstanceOptions"
	meta.__metatable = "The metatable is locked"
	meta.__tostring = function()
		return "SaveInstanceOptions"
	end
	meta.__index = function(_, key)
		if OPTION_TYPES[key] == nil then
			error(tostring(key) .. " is not a valid member of SaveInstanceOptions", 2)
		end
		if key == "IgnoreList" then
			return table.freeze(table.clone(values.IgnoreList))
		end
		if key == "DecompilerOptions" and not values.DecompilerOptions then
			values.DecompilerOptions = newDecompilerOptions()
		end
		return values[key]
	end
	meta.__newindex = function(_, key, value)
		local expected = OPTION_TYPES[key]
		if expected == nil then
			error(tostring(key) .. " is not a valid member of SaveInstanceOptions", 2)
		end
		if type(value) ~= expected then
			error(string.format("invalid value for SaveInstanceOptions.%s (%s expected, got %s)", key, expected, typeof(value)), 2)
		end
		if key == "IgnoreList" then
			local list = {}
			for _, item in value do
				if typeof(item) ~= "Instance" then
					error("SaveInstanceOptions.IgnoreList must only contain Instances", 2)
				end
				table.insert(list, item)
			end
			value = list
		end
		values[key] = value
	end

	return object
end

-- USSI, loaded once into an env of its own. warn and decompile are routed
-- through the current save so failures and DecompilerOptions reach it.
local saver
local current

local function getSaver()
	if saver then
		return saver
	end

	local ok, source = pcall(game.HttpGet, game, USSI_URL, true)
	if not ok or type(source) ~= "string" or #source == 0 then
		error("could not download the serializer: " .. tostring(source), 0)
	end

	local chunk, err = loadstring(source, "saveinstance")
	if not chunk then
		error("could not compile the serializer: " .. tostring(err), 0)
	end

	local env = setmetatable({
		warn = function(...)
			if current then
				local parts = table.pack(...)
				for i = 1, parts.n do
					parts[i] = tostring(parts[i])
				end
				table.insert(current.warnings, table.concat(parts, " "))
			end
			return realWarn(...)
		end,
		decompile = function(script)
			local state = current
			if state and state.decompiled then
				local ok, bytecode = pcall(getscriptbytecode, script, true)
				if ok and type(bytecode) == "string" then
					local source = state.decompiled[bytecode]
					if source then
						local okName, path = pcall(script.GetFullName, script)
						return LOGO .. "\n *  Script Path: " .. (okName and path or script.Name) .. "\n]]\n\n" .. source
					end
					local reason = state.failed[bytecode]
					if reason then
						error(reason, 0)
					end
				end
			end
			local decompile = genv.decompile
			if current and current.decompilerOptions then
				return decompile(script, current.decompilerOptions)
			end
			return decompile(script)
		end,
	}, { __index = genv })
	setfenv(chunk, env)

	local result = chunk()
	if type(result) ~= "function" then
		error("the serializer did not load", 0)
	end

	saver = result
	return saver
end

local function ensureParentFolders(path)
	local parent = ""
	for segment in string.gmatch(path, "([^/\\]+)[/\\]") do
		parent = parent == "" and segment or (parent .. "/" .. segment)
		if not isfolder(parent) then
			makefolder(parent)
		end
	end
end

local function generatedPath(isModel)
	local base = (isModel and "model " or "place ") .. tostring(game.PlaceId)
	local extension = isModel and ".rbxm" or ".rbxl"
	local path = base .. extension
	local counter = 0
	while isfile(path) do
		counter += 1
		path = base .. " (" .. counter .. ")" .. extension
	end
	return path
end

-- Extended: plain option tables, as other executors and testers pass them
-- (saveinstance(part, { FileName = "x" })). Keys are matched case-insensitively,
-- under Volt's names or the usual USSI/UNC ones; unknown keys and values of
-- the wrong type are ignored.
local TABLE_ALIASES = {
	filepath = "FilePath",
	filename = "FilePath",
	ignorearchivable = "IgnoreArchivable",
	ignorenotarchivable = "IgnoreArchivable",
	saveplayercharacters = "SavePlayerCharacters",
	savecharacters = "SavePlayerCharacters",
	saveplayers = "SavePlayers",
	isolateplayers = "SavePlayers",
	disablecompression = "DisableCompression",
	decompilescripts = "DecompileScripts",
	decompile = "DecompileScripts",
	savenoncreatable = "SaveNonCreatable",
	savenotcreatable = "SaveNonCreatable",
	savenilinstances = "SaveNilInstances",
	nilinstances = "SaveNilInstances",
	copytoclipboard = "CopyToClipboard",
	clipboard = "CopyToClipboard",
	ignorelist = "IgnoreList",
	instancesblacklist = "IgnoreList",
	decompileroptions = "DecompilerOptions",
	showstatus = "ShowStatus",
	showprogress = "ShowStatus",
	statustext = "ShowStatus",
	safemode = "SafeMode",
	savemeshes = "SaveMeshes",
	meshes = "SaveMeshes",
	exportmeshes = "SaveMeshes",
}

local function optionsFromTable(source)
	local object = SaveInstanceOptions.new()
	for key, value in source do
		if type(key) ~= "string" then
			continue
		end
		local lowered = string.lower(key)
		if lowered == "noscripts" and type(value) == "boolean" then
			object.DecompileScripts = not value
			continue
		end
		local name = TABLE_ALIASES[lowered]
		if not name or type(value) ~= OPTION_TYPES[name] then
			continue
		end
		if name == "IgnoreList" then
			local list = {}
			for _, item in value do
				if typeof(item) == "Instance" then
					table.insert(list, item)
				end
			end
			value = list
		end
		object[name] = value
	end
	return object
end

local function isInstanceArray(value)
	if type(value) ~= "table" or #value == 0 then
		return false
	end
	for _, item in value do
		if typeof(item) ~= "Instance" then
			return false
		end
	end
	return true
end
)LUA") + SUPQ_OBF(R"LUA(
local NEVER_SAVED = { CoreGui = true, CorePackages = true }

local function isClientScript(instance, class)
	if class == "ModuleScript" then
		return true
	elseif class == "LocalScript" then
		return instance.RunContext ~= Enum.RunContext.Server
	elseif class == "Script" then
		return instance.RunContext == Enum.RunContext.Client
	end
	return false
end

local function meshIdOf(instance, class)
	local ok, id
	if class == "MeshPart" then
		ok, id = pcall(function()
			return instance.MeshId
		end)
	elseif class == "SpecialMesh" then
		ok, id = pcall(function()
			return instance.MeshType == Enum.MeshType.FileMesh and instance.MeshId or ""
		end)
	end
	if ok and type(id) == "string" and id ~= "" then
		return id
	end
	return nil
end

-- One pass over what the save will cover: client scripts (by bytecode) and mesh ids.
local function collect(isPlace, root, values)
	local blocked = {}
	for _, item in values.IgnoreList do
		blocked[item] = true
	end
	if not values.SavePlayerCharacters then
		for _, player in game:GetService("Players"):GetPlayers() do
			if player.Character then
				blocked[player.Character] = true
			end
		end
	end
	local skip = {}
	for item in blocked do
		skip[item] = true
		local ok, list = pcall(item.GetDescendants, item)
		if ok then
			for _, descendant in list do
				skip[descendant] = true
			end
		end
	end

	local scripts, bytecodes, meshes, meshSeen = {}, {}, {}, {}
	local wantScripts, wantMeshes = values.DecompileScripts, values.SaveMeshes

	local function consider(instance)
		if skip[instance] then
			return
		end
		local class = instance.ClassName
		if wantScripts and isClientScript(instance, class) then
			local ok, bytecode = pcall(getscriptbytecode, instance, true)
			if ok and type(bytecode) == "string" and #bytecode > 0 and not bytecodes[bytecode] then
				bytecodes[bytecode] = true
				scripts[#scripts + 1] = {
					name = string.sub((string.gsub(instance.Name, "[^%w_]", "")), 1, 64),
					bytecode = bytecode,
				}
			end
		elseif wantMeshes then
			local id = meshIdOf(instance, class)
			if id and not meshSeen[id] then
				meshSeen[id] = true
				meshes[#meshes + 1] = id
			end
		end
	end

	local function walk(container)
		consider(container)
		local ok, list = pcall(container.GetDescendants, container)
		if ok then
			for _, instance in list do
				consider(instance)
			end
		end
	end

	if isPlace then
		for _, service in game:GetChildren() do
			if not NEVER_SAVED[service.ClassName] and not blocked[service] then
				walk(service)
			end
		end
	elseif type(root) == "table" then
		for _, item in root do
			walk(item)
		end
	else
		walk(root)
	end

	if values.SaveNilInstances and getnilinstances then
		local ok, list = pcall(getnilinstances)
		if ok and type(list) == "table" then
			for _, instance in list do
				if typeof(instance) == "Instance" then
					walk(instance)
				end
			end
		end
	end

	return scripts, meshes
end

local function sendBatch(batch, query, decompiled, failed)
	local parts = table.create(#batch + 2)
	parts[1] = "SQB1"
	parts[2] = string.pack("<I4", #batch)
	for i, entry in batch do
		parts[i + 2] = string.pack("<s4s4", entry.name, entry.bytecode)
	end

	local ok, response = pcall(request, {
		Url = BATCH_URL .. query,
		Method = "POST",
		Headers = { ["content-type"] = "application/octet-stream" },
		Body = table.concat(parts),
	})
	if not ok or not response or response.StatusCode ~= 200 then
		return
	end

	local body = response.Body or ""
	if #body < 8 or string.sub(body, 1, 4) ~= "SQR1" then
		return
	end
	local parsed, results = pcall(function()
		local count, at = string.unpack("<I4", body, 5)
		if count ~= #batch then
			return nil
		end
		local list = table.create(count)
		for i = 1, count do
			local flag, text
			flag, text, at = string.unpack("<Bs4", body, at)
			list[i] = { flag == 1, text }
		end
		return list
	end)
	if not parsed or not results then
		return
	end

	for i, result in results do
		local bytecode = batch[i].bytecode
		if result[1] then
			decompiled[bytecode] = result[2]
		else
			failed[bytecode] = "decompile: " .. (string.match(result[2], "^%-%-%s*([^\n]*)") or result[2])
		end
	end
end

-- Every script in a handful of requests, decompiled in parallel by the server.
-- A batch that fails leaves its scripts out, and those fall back to decompile().
local function decompileAll(scripts, decompilerOptions)
	local decompiled, failed = {}, {}
	if #scripts == 0 then
		return decompiled, failed
	end

	local width = 0
	if decompilerOptions then
		local formatter = decompilerOptions.Formatter
		width = math.clamp(math.floor(formatter and formatter.IndentWidth or 0), 0, 255)
	end
	local query = "?indent_width=" .. width

	local batches, batch, size = {}, {}, 0
	for _, entry in scripts do
		if #batch > 0 and (#batch >= BATCH_SCRIPTS or size + #entry.bytecode > BATCH_BYTES) then
			batches[#batches + 1] = batch
			batch, size = {}, 0
		end
		batch[#batch + 1] = entry
		size += #entry.bytecode
	end
	batches[#batches + 1] = batch

	local nextBatch, running = 0, math.min(BATCHES_IN_FLIGHT, #batches)
	for _ = 1, running do
		task.spawn(function()
			while true do
				nextBatch += 1
				local current = batches[nextBatch]
				if not current then
					break
				end
				pcall(sendBatch, current, query, decompiled, failed)
			end
			running -= 1
		end)
	end
	while running > 0 do
		task.wait()
	end
	return decompiled, failed
end

local function meshToObj(mesh)
	local lines, n = {}, 0
	local function push(line)
		n += 1
		lines[n] = line
	end

	local positions = {}
	for i, id in mesh:GetVertices() do
		positions[id] = i
		local p = mesh:GetPosition(id)
		push(string.format("v %.6f %.6f %.6f", p.X, p.Y, p.Z))
	end

	local uvs, hasUVs = {}, false
	local okUVs, uvList = pcall(mesh.GetUVs, mesh)
	if okUVs and #uvList > 0 then
		hasUVs = true
		for i, id in uvList do
			uvs[id] = i
			local t = mesh:GetUV(id)
			push(string.format("vt %.6f %.6f", t.X, 1 - t.Y))
		end
	end

	local normals, hasNormals = {}, false
	local okNormals, normalList = pcall(mesh.GetNormals, mesh)
	if okNormals and #normalList > 0 then
		hasNormals = true
		for i, id in normalList do
			normals[id] = i
			local v = mesh:GetNormal(id)
			push(string.format("vn %.6f %.6f %.6f", v.X, v.Y, v.Z))
		end
	end

	for _, face in mesh:GetFaces() do
		local corners = mesh:GetFaceVertices(face)
		local faceUVs = hasUVs and select(2, pcall(mesh.GetFaceUVs, mesh, face))
		local faceNormals = hasNormals and select(2, pcall(mesh.GetFaceNormals, mesh, face))
		faceUVs = type(faceUVs) == "table" and faceUVs or nil
		faceNormals = type(faceNormals) == "table" and faceNormals or nil
		local parts = table.create(#corners + 1)
		parts[1] = "f"
		for k, vertex in corners do
			local v = positions[vertex]
			local t = faceUVs and uvs[faceUVs[k]]
			local vn = faceNormals and normals[faceNormals[k]]
			parts[k + 1] = if t and vn then `{v}/{t}/{vn}` elseif t then `{v}/{t}` elseif vn then `{v}//{vn}` else tostring(v)
		end
		push(table.concat(parts, " "))
	end

	return table.concat(lines, "\n") .. "\n"
end

local function meshFileName(id)
	local digits = string.match(id, "[?&]id=(%d+)") or string.match(id, "(%d+)%s*$")
	return (digits or string.gsub(id, "[^%w%-_%.]", "_")) .. ".obj"
end

local function loadEditableMesh(AssetService, id)
	local source = (Content and Content.fromUri) and Content.fromUri(id) or id
	local ok, mesh = pcall(AssetService.CreateEditableMeshAsync, AssetService, source, { FixedSize = true })
	if not ok then
		ok, mesh = pcall(AssetService.CreateEditableMeshAsync, AssetService, source)
	end
	if ok and mesh then
		return mesh
	end
	return nil, tostring(mesh)
end

-- Meshes go out as .obj (Studio's 3D Importer reads them), read through
-- EditableMesh, which the game must allow (Security > Mesh / Image APIs).
local function exportMeshes(ids, folder)
	local report = { saved = 0, failed = 0, reason = nil, done = false }
	if #ids == 0 then
		report.done = true
		return report
	end

	local AssetService = game:GetService("AssetService")
	local made = false
	local blocked = false
	local nextMesh, running = 0, math.min(MESH_WORKERS, #ids)

	local function exportOne(id)
		local mesh, reason = loadEditableMesh(AssetService, id)
		local ok, data
		if mesh then
			ok, data = pcall(meshToObj, mesh)
			pcall(mesh.Destroy, mesh)
			if not ok then
				reason = tostring(data)
			end
		end
		if not ok then
			report.failed += 1
			report.reason = report.reason or reason
			if reason and string.find(reason, "not accessible", 1, true) then
				blocked = true
			end
			return
		end
		if not made then
			made = true
			ensureParentFolders(folder .. "/")
		end
		writefile(folder .. "/" .. meshFileName(id), data)
		report.saved += 1
	end

	for _ = 1, running do
		task.spawn(function()
			while not blocked do
				nextMesh += 1
				local id = ids[nextMesh]
				if not id then
					break
				end
				local okExport, problem = pcall(exportOne, id)
				if not okExport then
					report.failed += 1
					report.reason = report.reason or tostring(problem)
				end
			end
			running -= 1
			if running == 0 then
				if blocked then
					report.failed = #ids - report.saved
				end
				report.done = true
			end
		end)
	end
	return report
end

local function targetPath(values, isPlace)
	local path = values.FilePath
	if path == "" then
		if values.CopyToClipboard then
			return nil
		end
		return generatedPath(not isPlace)
	elseif not string.match(path, "%.[^/\\]+$") then
		return path .. (isPlace and ".rbxl" or ".rbxm")
	end
	return path
end

local saveCounter = 0

local function save(fname, root, options)
	if type(options) == "table" then
		options = optionsFromTable(options)
	elseif options ~= nil and optionValues[options] == nil then
		error(string.format("invalid argument #%d to '%s' (SaveInstanceOptions or table expected, got %s)",
			fname == "saveplace" and 1 or 2, fname, typeof(options)), 3)
	end
	local values = optionValues[options or SaveInstanceOptions.new()]

	local isPlace = root == game
	if not isPlace and type(root) == "table" then
		if #root == 0 then
			error("invalid argument #1 to '" .. fname .. "' (array of Instances is empty)", 3)
		end
		for _, item in root do
			if typeof(item) ~= "Instance" then
				error("invalid argument #1 to '" .. fname .. "' (array must only contain Instances)", 3)
			end
		end
	elseif not isPlace and typeof(root) ~= "Instance" then
		error("invalid argument #1 to '" .. fname .. "' (Instance or {Instance} expected, got " .. typeof(root) .. ")", 3)
	end

	-- One save at a time; USSI drops overlapping calls.
	while current do
		task.wait()
	end
	local state = {
		warnings = {},
		decompilerOptions = values.DecompileScripts and values.DecompilerOptions or nil,
	}
	current = state

	local meshReport
	local ok, err = pcall(function()
		local ussi = getSaver()
		local path = targetPath(values, isPlace)

		if values.DecompileScripts or values.SaveMeshes then
			local scripts, meshes = collect(isPlace, root, values)
			if values.SaveMeshes then
				local folder = path and (string.gsub(path, "%.[^/\\%.]+$", "") .. " meshes") or ("meshes " .. tostring(game.PlaceId))
				meshReport = exportMeshes(meshes, folder)
			end
			if values.DecompileScripts then
				state.decompiled, state.failed = decompileAll(scripts, state.decompilerOptions)
			end
		end

		local ignoreList = { "CoreGui", "CorePackages", Packages = false }
		for _, item in values.IgnoreList do
			table.insert(ignoreList, item)
		end

		-- A unique name per call: USSI refuses a name it has already saved,
		-- even when the bytes only go to Callback.
		saveCounter += 1
		local internalName = "supq-save-" .. saveCounter .. (isPlace and ".rbxl" or ".rbxm")

		local data
		local ussiOptions = {
			Binary = true,
			CompressionMode = (not values.DisableCompression) and "zstd" or false,
			Decompile = values.DecompileScripts,
			IgnoreNotArchivable = values.IgnoreArchivable,
			SavePlayerCharacters = values.SavePlayerCharacters,
			IsolatePlayers = values.SavePlayers,
			SaveNotCreatable = values.SaveNonCreatable,
			NilInstances = values.SaveNilInstances,
			IgnoreList = ignoreList,
			FilePath = internalName,
			ShowStatus = values.ShowStatus ~= false,
			ReadMe = false,
			SafeMode = values.SafeMode == true,
			DecompileTimeout = -1,
			BytecodeTimeout = -1,
			scriptcache = false,
			CompressionLevel = 3,
			Callback = function(serialized)
				data = serialized
			end,
		}

		-- USSI's "a save is running" flag. Saves are serialized above, so a set
		-- flag is always left over from a save that died partway, and would make
		-- USSI return without doing anything from then on.
		if isPlace then
			ussi(ussiOptions)
		else
			ussiOptions.mode = "full"
			ussi(root, ussiOptions)
		end
		genv[internalName] = nil

		if type(data) ~= "string" then
			-- Every other way out of USSI only warns; hand those back.
			local reason
			for i, message in state.warnings do
				if string.find(message, "Error found while saving", 1, true) then
					reason = state.warnings[i + 1]
					break
				end
			end
			if not reason and #state.warnings > 0 then
				reason = table.concat(state.warnings, " | ", math.max(1, #state.warnings - 2))
			end
			error(reason or "nothing was serialized", 0)
		end

		if values.CopyToClipboard then
			setrbxclipboard(data)
		end

		if path then
			ensureParentFolders(path)
			writefile(path, data)
		end

		local message = "Saved instance to your /workspace directory"
		if meshReport then
			while not meshReport.done do
				task.wait()
			end
			if meshReport.saved > 0 then
				message ..= " (" .. meshReport.saved .. " meshes)"
			end
			if meshReport.failed > 0 then
				realWarn(string.format("%s: %d of %d meshes were not saved: %s", fname, meshReport.failed,
					meshReport.saved + meshReport.failed, tostring(meshReport.reason)))
			end
		end
		if path and createToast then
			pcall(createToast, "Saved Instance", message, "success")
		end
	end)

	current = nil
	if not ok then
		error(fname .. " failed: " .. tostring(err), 3)
	end
end

-- supQ: saveinstance(root: Instance | {Instance}, options: SaveInstanceOptions?) -> ()
-- Extended: no root saves the current place, as on other executors, and
-- saveinstance(options) is saveinstance(game, options), options being either a
-- SaveInstanceOptions or a plain table.
local function saveinstance(root, options)
	if root == nil then
		root = game
	elseif options == nil and (optionValues[root] ~= nil or (type(root) == "table" and not isInstanceArray(root))) then
		root, options = game, root
	end
	save("saveinstance", root, options)
end

-- supQ: saveplace(options: SaveInstanceOptions?) -> ()
local function saveplace(options)
	save("saveplace", game, options)
end

getgenv().SaveInstanceOptions = table.freeze(SaveInstanceOptions)
getgenv().saveinstance = saveinstance
getgenv().saveplace = saveplace)LUA");

    std::string Compat = SUPQ_OBF(R"LUA(local genv = getgenv()

-- RakNet.OnPacketSend / OnPacketReceive (docs.projectreal.gg)
local raknet = genv.raknet
if type(raknet) == "table" then
	local function packetSignal(add, remove)
		local signal = {}
		function signal.Connect(first, second)
			local callback = if type(first) == "function" then first else second
			if type(callback) ~= "function" then
				error("Connect expects a function", 2)
			end
			local connection = { Connected = true }
			local function hook(packet)
				if connection.Connected then
					callback(packet)
				end
			end
			add(hook)
			function connection.Disconnect()
				if connection.Connected then
					connection.Connected = false
					remove(hook)
				end
			end
			connection.disconnect = connection.Disconnect
			return connection
		end
		signal.connect = signal.Connect
		return table.freeze(signal)
	end
	raknet.OnPacketSend = packetSignal(raknet.add_send_hook, raknet.remove_send_hook)
	raknet.OnPacketReceive = packetSignal(raknet.add_receive_hook, raknet.remove_receive_hook)
	genv.RakNet = raknet
end

-- json library (docs.projectreal.gg/en/reference/json)
local json = {}

local kinds = setmetatable({}, { __mode = "k" })  -- table -> "array" | "object"
local orders = setmetatable({}, { __mode = "k" }) -- decoded object -> key order

local nullMeta = table.freeze({ __tostring = function() return "null" end })
local null = table.freeze(setmetatable({}, nullMeta))
json.null = null

local sbyte, schar, sfind, ssub, sformat, sgsub = string.byte, string.char, string.find, string.sub, string.format, string.gsub
local concat, insert, sort = table.concat, table.insert, table.sort
local floor, huge = math.floor, math.huge

local function isArrayTable(value, emptyAsArray)
	local kind = kinds[value]
	if kind then
		return kind == "array"
	end
	local count = 0
	for key in next, value do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return false
		end
		count += 1
	end
	if count == 0 then
		return emptyAsArray == true
	end
	return rawlen(value) == count
end

local function clampDepth(depth, default)
	if type(depth) ~= "number" then
		return default
	end
	return math.clamp(floor(depth), 1, 4096)
end

local escapes = {
	['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b", ["\f"] = "\\f",
	["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t",
}
local function escapeString(text)
	return (sgsub(text, '[%c"\\]', function(c)
		return escapes[c] or sformat("\\u%04x", sbyte(c))
	end))
end

local function encodeNumber(value, options)
	if value ~= value or value == huge or value == -huge then
		if options.encodeInvalidNumbersAsNull then
			return "null"
		end
		error("cannot encode " .. tostring(value) .. " as JSON", 0)
	end
	if value % 1 == 0 and value > -2^53 and value < 2^53 then
		return sformat("%d", value)
	end
	for precision = 15, 17 do
		local text = sformat("%." .. precision .. "g", value)
		if tonumber(text) == value then
			return text
		end
	end
	return sformat("%.17g", value)
end

local function readEncodeOptions(options, forcePretty)
	local result = {
		pretty = forcePretty == true,
		indent = "  ",
		sortKeys = false,
		emptyTableAsArray = false,
		errorOnUnsupported = false,
		encodeInvalidNumbersAsNull = false,
		maxDepth = 512,
	}
	local kind = type(options)
	if kind == "boolean" then
		result.pretty = options or result.pretty
	elseif kind == "number" then
		result.pretty = true
		result.indent = string.rep(" ", math.clamp(floor(options), 0, 16))
	elseif kind == "string" then
		result.pretty = true
		result.indent = ssub(options, 1, 32)
	elseif kind == "table" then
		if options.pretty ~= nil then
			result.pretty = forcePretty == true or options.pretty == true
		end
		if type(options.indent) == "string" then
			result.indent = ssub(options.indent, 1, 32)
		elseif type(options.indent) == "number" then
			result.indent = string.rep(" ", math.clamp(floor(options.indent), 0, 16))
		end
		result.sortKeys = options.sortKeys == true
		result.emptyTableAsArray = options.emptyTableAsArray == true
		result.errorOnUnsupported = options.errorOnUnsupported == true
		result.encodeInvalidNumbersAsNull = options.encodeInvalidNumbersAsNull == true
		result.maxDepth = clampDepth(options.maxDepth, 512)
	elseif options ~= nil then
		error("invalid options (expected boolean, number, string or table)", 0)
	end
	return result
end

local function objectKeys(value, sortKeys)
	local keys, seen = {}, {}
	local order = orders[value]
	if order and not sortKeys then
		for _, key in ipairs(order) do
			if rawget(value, key) ~= nil and not seen[key] then
				seen[key] = true
				insert(keys, key)
			end
		end
	end
	local extra = {}
	for key in next, value do
		if not seen[key] then
			insert(extra, key)
		end
	end
	if sortKeys or order then
		sort(extra, function(a, b)
			return tostring(a) < tostring(b)
		end)
	end
	for _, key in ipairs(extra) do
		insert(keys, key)
	end
	return keys
end

local function encodeValue(value, options, depth, stack, out)
	local kind = type(value)
	if value == nil or value == null then
		insert(out, "null")
	elseif kind == "boolean" then
		insert(out, value and "true" or "false")
	elseif kind == "number" then
		insert(out, encodeNumber(value, options))
	elseif kind == "string" then
		insert(out, '"' .. escapeString(value) .. '"')
	elseif kind == "table" then
		if stack[value] then
			error("cannot encode a table that contains itself", 0)
		end
		if depth > options.maxDepth then
			error("maximum nesting depth exceeded", 0)
		end
		stack[value] = true
		local newline = options.pretty and "\n" or ""
		local inner = options.pretty and string.rep(options.indent, depth) or ""
		local outer = options.pretty and string.rep(options.indent, depth - 1) or ""
		if isArrayTable(value, options.emptyTableAsArray) then
			local length = rawlen(value)
			if length == 0 then
				insert(out, "[]")
			else
				insert(out, "[" .. newline)
				for i = 1, length do
					insert(out, inner)
					encodeValue(rawget(value, i), options, depth + 1, stack, out)
					insert(out, (i < length and "," or "") .. newline)
				end
				insert(out, outer .. "]")
			end
		else
			local keys = objectKeys(value, options.sortKeys)
			local written = 0
			insert(out, "{" .. newline)
			for _, key in ipairs(keys) do
				local keyKind = type(key)
				local name
				if keyKind == "string" then
					name = key
				elseif keyKind == "number" then
					name = encodeNumber(key, options)
				elseif options.errorOnUnsupported then
					error("cannot encode a " .. keyKind .. " key", 0)
				end
				if name then
					if written > 0 then
						insert(out, "," .. newline)
					end
					written += 1
					insert(out, inner .. '"' .. escapeString(name) .. '":' .. (options.pretty and " " or ""))
					encodeValue(rawget(value, key), options, depth + 1, stack, out)
				end
			end
			if written == 0 then
				out[#out] = "{}"
			else
				insert(out, newline .. outer .. "}")
			end
		end
		stack[value] = nil
	elseif options.errorOnUnsupported then
		error("cannot encode a value of type " .. typeof(value), 0)
	else
		insert(out, "null")
	end
end

local function encode(name, value, options, forcePretty)
	local ok, result = pcall(function()
		local parsed = readEncodeOptions(options, forcePretty)
		local out = {}
		encodeValue(value, parsed, 1, {}, out)
		return concat(out)
	end)
	if not ok then
		error(name .. ": " .. tostring(result), 3)
	end
	return result
end

-- Decoder. Errors carry the byte position so the safe variants can return it.
local ParseError = {}

local function fail(message, position)
	error(setmetatable({ message = message, position = position }, ParseError), 0)
end

local function utf8Encode(code)
	return utf8.char(code)
end

local unescapes = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }

local function parseString(text, position)
	-- `position` is the opening quote
	local parts = {}
	local index = position + 1
	while true do
		local stop = sfind(text, '["\\%c]', index)
		if not stop then
			fail("unterminated string", position)
		end
		if stop > index then
			insert(parts, ssub(text, index, stop - 1))
		end
		local c = sbyte(text, stop)
		if c == 34 then
			return concat(parts), stop + 1
		elseif c ~= 92 then
			fail("control character in string", stop)
		end
		local e = ssub(text, stop + 1, stop + 1)
		if e == "u" then
			local hex = ssub(text, stop + 2, stop + 5)
			local code = #hex == 4 and sfind(hex, "^%x%x%x%x$") and tonumber(hex, 16)
			if not code then
				fail("invalid \\u escape", stop)
			end
			index = stop + 6
			if code >= 0xD800 and code <= 0xDBFF then
				local low = ssub(text, index, index + 1) == "\\u" and tonumber(ssub(text, index + 2, index + 5), 16)
				if low and low >= 0xDC00 and low <= 0xDFFF then
					code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
					index += 6
				else
					code = 0xFFFD
				end
			elseif code >= 0xDC00 and code <= 0xDFFF then
				code = 0xFFFD
			end
			insert(parts, utf8Encode(code))
		else
			local replacement = unescapes[e]
			if not replacement then
				fail("invalid escape sequence", stop)
			end
			insert(parts, replacement)
			index = stop + 2
		end
	end
end

local function skip(text, position)
	return sfind(text, "[^ \t\r\n]", position) or #text + 1
end

local parseValue

local function parseArray(text, position, options, depth)
	if depth > options.maxDepth then
		fail("maximum nesting depth exceeded", position)
	end
	local result = {}
	kinds[result] = "array"
	local count = 0
	position = skip(text, position + 1)
	if sbyte(text, position) == 93 then
		return result, position + 1
	end
	while true do
		local value
		value, position = parseValue(text, position, options, depth + 1)
		count += 1
		result[count] = value
		position = skip(text, position)
		local c = sbyte(text, position)
		if c == 93 then
			return result, position + 1
		elseif c ~= 44 then
			fail("expected ',' or ']'", position)
		end
		position = skip(text, position + 1)
	end
end
)LUA" R"LUA(
local function parseObject(text, position, options, depth)
	if depth > options.maxDepth then
		fail("maximum nesting depth exceeded", position)
	end
	local result, order = {}, {}
	kinds[result] = "object"
	orders[result] = order
	position = skip(text, position + 1)
	if sbyte(text, position) == 125 then
		return result, position + 1
	end
	while true do
		if sbyte(text, position) ~= 34 then
			fail("expected string key", position)
		end
		local key
		key, position = parseString(text, position)
		position = skip(text, position)
		if sbyte(text, position) ~= 58 then
			fail("expected ':'", position)
		end
		local value
		value, position = parseValue(text, skip(text, position + 1), options, depth + 1)
		if result[key] == nil then
			insert(order, key)
		end
		result[key] = value
		position = skip(text, position)
		local c = sbyte(text, position)
		if c == 125 then
			return result, position + 1
		elseif c ~= 44 then
			fail("expected ',' or '}'", position)
		end
		position = skip(text, position + 1)
	end
end

function parseValue(text, position, options, depth)
	position = skip(text, position)
	local c = sbyte(text, position)
	if c == 123 then
		return parseObject(text, position, options, depth)
	elseif c == 91 then
		return parseArray(text, position, options, depth)
	elseif c == 34 then
		return parseString(text, position)
	elseif c == 116 and ssub(text, position, position + 3) == "true" then
		return true, position + 4
	elseif c == 102 and ssub(text, position, position + 4) == "false" then
		return false, position + 5
	elseif c == 110 and ssub(text, position, position + 3) == "null" then
		return (if options.useNull then null else nil), position + 4
	elseif c == 45 or (c and c >= 48 and c <= 57) then
		local first, last = sfind(text, "^-?[%d%.eE+-]*", position)
		local literal = ssub(text, first, last)
		local digits, tail = string.match(literal, "^%-?(%d+)(.*)$")
		if not digits or (#digits > 1 and sbyte(digits, 1) == 48) then
			fail("invalid number", position)
		end
		if sbyte(tail, 1) == 46 then
			local fraction
			fraction, tail = string.match(tail, "^%.(%d+)(.*)$")
			if not fraction then
				fail("invalid number", position)
			end
		end
		if tail ~= "" and not string.match(tail, "^[eE][-+]?%d+$") then
			fail("invalid number", position)
		end
		return tonumber(literal), last + 1
	elseif c == nil then
		fail("unexpected end of input", position)
	end
	fail("unexpected character '" .. schar(c) .. "'", position)
end

local function readDecodeOptions(options)
	local result = { useNull = true, maxDepth = 512 }
	if type(options) == "boolean" then
		result.useNull = options
	elseif type(options) == "table" then
		if options.useNull ~= nil then
			result.useNull = options.useNull == true
		end
		result.maxDepth = clampDepth(options.maxDepth, 512)
	elseif options ~= nil then
		error("invalid options (expected boolean or table)", 3)
	end
	return result
end

local function decodeRaw(text, options)
	local value, position = parseValue(text, 1, options, 1)
	position = skip(text, position)
	if position <= #text then
		fail("trailing characters after JSON value", position)
	end
	return value
end

local function protectedDecode(name, text, options)
	if type(text) ~= "string" then
		error(name .. ": argument #1 expected string, got " .. typeof(text), 3)
	end
	local parsed = readDecodeOptions(options)
	local ok, result = pcall(decodeRaw, text, parsed)
	if ok then
		return true, result
	end
	if getmetatable(result) == ParseError then
		return false, nil, sformat("%s: %s near position %d", name, result.message, result.position), result.position
	end
	error(result, 0)
end

local function decode(name, text, options)
	local ok, value, message = protectedDecode(name, text, options)
	if not ok then
		error(message, 3)
	end
	return value
end

function json.encode(value, options) return encode("json.encode", value, options, false) end
function json.stringify(value, options) return encode("json.stringify", value, options, false) end
function json.encodePretty(value, options) return encode("json.encodePretty", value, options, true) end
json.pretty = json.encodePretty

function json.decode(text, options) return decode("json.decode", text, options) end
function json.parse(text, options) return decode("json.parse", text, options) end

function json.decodeSafe(text, options)
	return protectedDecode("json.decodeSafe", text, options)
end
json.tryDecode = json.decodeSafe

function json.validate(text, options)
	local ok, _, message, position = protectedDecode("json.validate", text, options)
	if ok then
		return true
	end
	return false, message, position
end

function json.format(text, options)
	return encode("json.format", decode("json.format", text, true), options, true)
end

function json.minify(text)
	return encode("json.minify", decode("json.minify", text, true), false, false)
end

function json.escape(text)
	if type(text) ~= "string" then
		error("json.escape: argument #1 expected string, got " .. typeof(text), 2)
	end
	return escapeString(text)
end

function json.quote(text)
	if type(text) ~= "string" then
		error("json.quote: argument #1 expected string, got " .. typeof(text), 2)
	end
	return '"' .. escapeString(text) .. '"'
end

function json.unescape(text)
	if type(text) ~= "string" then
		error("json.unescape: argument #1 expected string, got " .. typeof(text), 2)
	end
	local literal = text
	local quoted = #text >= 2 and sbyte(text, 1) == 34 and sbyte(text, #text) == 34
	if not quoted then
		literal = '"' .. text .. '"'
	end
	local ok, result, position = pcall(parseString, literal, 1)
	if not ok then
		if getmetatable(result) == ParseError then
			error(sformat("json.unescape: %s near position %d", result.message, result.position), 2)
		end
		error(result, 0)
	end
	if position <= #literal then
		error(sformat("json.unescape: trailing characters near position %d", position), 2)
	end
	return result
end

function json.array(source)
	local result = {}
	if source ~= nil then
		if type(source) ~= "table" then
			error("json.array: argument #1 expected table, got " .. typeof(source), 2)
		end
		for i = 1, #source do
			result[i] = source[i]
		end
	end
	kinds[result] = "array"
	return result
end

function json.object(source)
	local result = {}
	if source ~= nil then
		if type(source) ~= "table" then
			error("json.object: argument #1 expected table, got " .. typeof(source), 2)
		end
		for key, value in next, source do
			result[key] = value
		end
	end
	kinds[result] = "object"
	return result
end

function json.isNull(value)
	return value == null
end

function json.isArray(value)
	return type(value) == "table" and value ~= null and isArrayTable(value, false)
end

function json.isObject(value)
	return type(value) == "table" and value ~= null and not isArrayTable(value, false)
end

function json.type(value)
	if value == null then
		return "null"
	elseif type(value) == "table" then
		return if isArrayTable(value, false) then "array" else "object"
	end
	return typeof(value)
end

function json.keys(object)
	if type(object) ~= "table" then
		error("json.keys: argument #1 expected table, got " .. typeof(object), 2)
	end
	local result = json.array()
	for _, key in ipairs(objectKeys(object, false)) do
		insert(result, key)
	end
	return result
end

function json.values(object)
	if type(object) ~= "table" then
		error("json.values: argument #1 expected table, got " .. typeof(object), 2)
	end
	local result = json.array()
	for _, key in ipairs(objectKeys(object, false)) do
		insert(result, object[key])
	end
	return result
end

function json.getPath(object, path, default)
	if type(object) ~= "table" then
		error("json.getPath: argument #1 expected table, got " .. typeof(object), 2)
	end
	local keys = {}
	if type(path) == "string" then
		for token in string.gmatch(path, "[^%.]+") do
			insert(keys, tonumber(token) and tonumber(token) % 1 == 0 and tonumber(token) or token)
		end
	elseif type(path) == "table" then
		for i = 1, #path do
			keys[i] = path[i]
		end
	else
		error("json.getPath: argument #2 expected string or table, got " .. typeof(path), 2)
	end
	local current = object
	for _, key in ipairs(keys) do
		if type(current) ~= "table" or current == null then
			return default
		end
		current = rawget(current, key)
		if current == nil then
			return default
		end
	end
	return current
end
json.getpath = json.getPath

function json.clone(value)
	if value == nil or type(value) ~= "table" then
		return value
	end
	return decode("json.clone", encode("json.clone", value, nil, false), true)
end

genv.json = table.freeze(json)
)LUA");

    std::string Toast = "";
}
