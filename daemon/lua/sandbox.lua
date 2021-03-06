-- Units
kB = 1024
MB = 1024*kB
GB = 1024*MB
-- Time
sec = 1000
minute = 60 * sec
hour = 60 * minute

-- Function aliases
-- `env.VAR returns os.getenv(VAR)`
env = {}
setmetatable(env, {
	__index = function (t, k) return os.getenv(k) end
})

-- Quick access to interfaces
-- `net.<iface>` => `net.interfaces()[iface]`
-- `net = {addr1, ..}` => `net.listen(name, addr1)`
setmetatable(net, {
	__index = function (t, k)
		local v = rawget(t, k)
		if v then return v
		else return net.interfaces()[k]
		end
	end,
	__newindex = function (t,k,v)
		local iname = rawget(net.interfaces(), v)
		if iname then t.listen(iname)
		else t.listen(v)
		end
	end
})

-- Syntactic sugar for module loading
-- `modules.<name> = <config>`
setmetatable(modules, {
	__newindex = function (t,k,v)
		if type(k) == 'number' then k = v end
		if not rawget(_G, k) then
			modules.load(k)
			local mod = rawget(_G, k)
			if k ~= v and mod and mod['config'] then
				mod['config'](v)
			end

		end
	end
})

-- Syntactic sugar for cache
-- `cache.{size|storage} = value`
setmetatable(cache, {
	__newindex = function (t,k,v)
		-- Defaults
		local storage = rawget(t, 'current_storage')
		if not storage then storage = 'lmdb://' end
		local size = rawget(t, 'current_size')
		if not size then size = 10*MB end
		-- Declarative interface for cache
		if     k == 'size'    then t.open(v, storage)
		elseif k == 'storage' then t.open(size, v)
		else   rawset(t, k, v) end
	end
})

-- Register module in Lua environment
function modules_register(module)
	-- Syntactic sugar for get() and set() properties
	setmetatable(module, {
		__index = function (t, k)
			local  v = rawget(t, k)
			if     v     then return v
			elseif rawget(t, 'get') then return t.get(k)
			end
		end,
		__newindex = function (t, k, v)
			local  old_v = rawget(t, k)
			if not old_v and rawget(t, 'set') then
				t.set(k..' '..v)
			end
		end
	})
end

-- Make sandboxed environment
local function make_sandbox(defined)
	local __protected = { modules = true, cache = true, net = true }
	return setmetatable({}, {
		__index = defined,
		__newindex = function (t, k, v)
			if __protected[k] then
				for k2,v2 in pairs(v) do
					defined[k][k2] = v2
				end
			else
				defined[k] = v
			end
		end
	})
end

-- Compatibility sandbox
if setfenv then -- Lua 5.1 and less
	_G = make_sandbox(getfenv(0))
	setfenv(0, _G)
else -- Lua 5.2+
	_SANDBOX = make_sandbox(_ENV)
end

-- Interactive command evaluation
function eval_cmd(line)
	-- Compatibility sandbox code loading
	local function load_code(code)
	    if getfenv then -- Lua 5.1
	        return loadstring(code)
	    else            -- Lua 5.2+
	        return load(code, nil, 't', _ENV)
	    end
	end
	local status, err, chunk
	chunk, err = load_code('return table_print('..line..')')
	if err then
		chunk, err = load_code(line)
	end
	if not err then
		return chunk()
	else
		error(err)
	end
end

-- Pretty printing
function table_print (tt, indent, done)
	done = done or {}
	indent = indent or 0
	result = ""
	if type(tt) == "table" then
		for key, value in pairs (tt) do
			result = result .. string.rep (" ", indent)
			if type (value) == "table" and not done [value] then
				done [value] = true
				result = result .. string.format("[%s] => {\n", tostring (key))
				result = result .. table_print (value, indent + 4, done)
				result = result .. string.rep (" ", indent)
				result = result .. "}\n"
			else
				result = result .. string.format("[%s] => %s\n",
				         tostring (key), tostring(value))
			end
		end
	else
		result = result .. tostring(tt) .. "\n"
	end
	return result
end
