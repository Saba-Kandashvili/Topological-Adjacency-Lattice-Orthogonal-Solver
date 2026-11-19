-- =================================================
-- CONFIGURATION
-- =================================================
local DEFAULT_IMAGE_DIR = "tile_mock/"
local DEFAULT_TILESET_FILE = "../tileset.json"
local TILESETS_ROOT_DIR = "tilesets"
local PROC_TILE_PX = 32

local WINDOW_BG_COLOR = {0.05, 0.05, 0.06}
local VOID_COLOR = {0.10, 0.02, 0.10}
local SOLID_TILE_COLOR = {0.25, 0.25, 0.28}
local UNCOLLAPSED_COLOR = {0.85, 0.15, 0.85}
local REACHABLE_COLOR = {0.0, 0.6, 0.2, 0.48}
local UNREACHABLE_COLOR = {0.6, 0.1, 0.2, 0.58}
local UNCOLLAPSED_TILE = { uncollapsed = true }

-- =================================================
-- PATH, OS & STRING HELPERS
-- =================================================

local function isWindowsHost()
    return package.config:sub(1, 1) == "\\"
end

local function normalizePath(p)
    if not p then return "" end
    p = p:gsub("\\", "/")
    p = p:gsub("/+", "/")
    p = p:gsub("^%./", "")
    return p
end

local function pathDir(p)
    p = normalizePath(p)
    local dir = p:match("^(.*)/[^/]+$")
    return dir or ""
end

local function pathStem(p)
    p = normalizePath(p)
    local base = p:match("([^/]+)$") or p
    return (base:gsub("%.[^%.]+$", ""))
end

local function joinPath(...)
    local parts = {}
    for i = 1, select("#", ...) do
        local s = select(i, ...)
        if s and s ~= "" then
            s = normalizePath(s):gsub("/+$", "")
            if s ~= "" then
                parts[#parts + 1] = s
            end
        end
    end
    return table.concat(parts, "/")
end

local function sanitizeSlug(s)
    local slug = tostring(s or "tileset"):lower():gsub("[^%w_%-]+", "_")
    slug = slug:gsub("^_+", ""):gsub("_+$", "")
    return (slug ~= "") and slug or "tileset"
end

local function makeDirRecursive(dir)
    if not dir or dir == "" then return false end
    local norm = normalizePath(dir)
    if isWindowsHost() then
        local winPath = norm:gsub("/", "\\")
        local ok = os.execute(string.format('if not exist "%s" mkdir "%s" >nul 2>nul', winPath, winPath))
        return ok == true or ok == 0
    else
        local escaped = norm:gsub("'", "'\\''")
        local ok = os.execute(string.format("mkdir -p '%s'", escaped))
        return ok == true or ok == 0
    end
end

local function readTextFile(path)
    if not path or path == "" then return nil end
    local norm = normalizePath(path)
    if love and love.filesystem and love.filesystem.getInfo and love.filesystem.getInfo(norm) then
        return love.filesystem.read(norm)
    end
    local f = io.open(path, "rb")
    if not f and norm ~= path then
        f = io.open(norm, "rb")
    end
    if not f then return nil end
    local data = f:read("*all")
    f:close()
    return data
end

-- =================================================
-- EXACT 64-BIT DECIMAL POWER-OF-2 + JSON DECODER
-- =================================================

local function decimalDoubleStr(s)
    local carry = 0
    local rev = {}
    for i = #s, 1, -1 do
        local d = tonumber(s:sub(i, i)) * 2 + carry
        carry = math.floor(d / 10)
        rev[#rev + 1] = tostring(d % 10)
    end
    if carry > 0 then
        rev[#rev + 1] = tostring(carry)
    end
    local out = {}
    for i = #rev, 1, -1 do
        out[#out + 1] = rev[i]
    end
    return table.concat(out)
end

local function buildPow2Strings(maxIndex)
    local t = { [0] = "1" }
    for i = 1, maxIndex do
        t[i] = decimalDoubleStr(t[i - 1])
    end
    return t
end

-- Minimal pure-Lua recursive-descent JSON parser
local function decodeJSON(str)
    local pos = 1
    local len = #str

    local function skipWS()
        while pos <= len do
            local b = str:byte(pos)
            if b == 32 or b == 9 or b == 10 or b == 13 then
                pos = pos + 1
            else
                break
            end
        end
    end

    local parseValue

    local function encodeUTF8(cp)
        if cp <= 0x7F then
            return string.char(cp)
        elseif cp <= 0x7FF then
            return string.char(0xC0 + math.floor(cp / 64), 0x80 + (cp % 64))
        else
            return string.char(
                0xE0 + math.floor(cp / 4096),
                0x80 + (math.floor(cp / 64) % 64),
                0x80 + (cp % 64)
            )
        end
    end

    local function parseString()
        pos = pos + 1
        local out = {}
        while pos <= len do
            local c = str:sub(pos, pos)
            if c == '"' then
                pos = pos + 1
                return table.concat(out)
            elseif c == '\\' then
                pos = pos + 1
                local esc = str:sub(pos, pos)
                if esc == '"' or esc == '\\' or esc == '/' then
                    out[#out + 1] = esc
                elseif esc == 'b' then out[#out + 1] = '\b'
                elseif esc == 'f' then out[#out + 1] = '\f'
                elseif esc == 'n' then out[#out + 1] = '\n'
                elseif esc == 'r' then out[#out + 1] = '\r'
                elseif esc == 't' then out[#out + 1] = '\t'
                elseif esc == 'u' then
                    local hex = str:sub(pos + 1, pos + 4)
                    local cp = tonumber(hex, 16)
                    if not cp then error("Invalid \\u escape at position " .. pos) end
                    out[#out + 1] = encodeUTF8(cp)
                    pos = pos + 4
                else
                    error("Invalid escape \\" .. esc .. " at position " .. pos)
                end
                pos = pos + 1
            else
                out[#out + 1] = c
                pos = pos + 1
            end
        end
        error("Unterminated JSON string")
    end

    local function parseNumber()
        local s = str:match("^-?%d+%.?%d*[eE]?[+-]?%d*", pos)
        if not s or #s == 0 then error("Invalid JSON number at position " .. pos) end
        pos = pos + #s
        local n = tonumber(s)
        if not n then error("Failed to parse number: " .. s) end
        return n
    end

    local function parseArray()
        pos = pos + 1
        local arr = {}
        skipWS()
        if str:sub(pos, pos) == ']' then
            pos = pos + 1
            return arr
        end
        local idx = 1
        while pos <= len do
            arr[idx] = parseValue()
            idx = idx + 1
            skipWS()
            local ch = str:sub(pos, pos)
            if ch == ',' then
                pos = pos + 1
            elseif ch == ']' then
                pos = pos + 1
                return arr
            else
                error("Expected ',' or ']' in array at position " .. pos)
            end
        end
        error("Unterminated JSON array")
    end

    local function parseObject()
        pos = pos + 1
        local obj = {}
        skipWS()
        if str:sub(pos, pos) == '}' then
            pos = pos + 1
            return obj
        end
        while pos <= len do
            skipWS()
            if str:sub(pos, pos) ~= '"' then
                error("Expected string key at position " .. pos)
            end
            local k = parseString()
            skipWS()
            if str:sub(pos, pos) ~= ':' then
                error("Expected ':' at position " .. pos)
            end
            pos = pos + 1
            obj[k] = parseValue()
            skipWS()
            local ch = str:sub(pos, pos)
            if ch == ',' then
                pos = pos + 1
            elseif ch == '}' then
                pos = pos + 1
                return obj
            else
                error("Expected ',' or '}' in object at position " .. pos)
            end
        end
        error("Unterminated JSON object")
    end

    parseValue = function()
        skipWS()
        local ch = str:sub(pos, pos)
        if ch == '"' then return parseString()
        elseif ch == '{' then return parseObject()
        elseif ch == '[' then return parseArray()
        elseif ch == '-' or (ch >= '0' and ch <= '9') then return parseNumber()
        elseif str:sub(pos, pos + 3) == "true" then pos = pos + 4; return true
        elseif str:sub(pos, pos + 4) == "false" then pos = pos + 5; return false
        elseif str:sub(pos, pos + 3) == "null" then pos = pos + 4; return nil
        else
            error("Unexpected JSON token at position " .. pos .. ": " .. ch)
        end
    end

    return parseValue()
end

-- =================================================
-- COLORS, SPRITE LOADING & PROCEDURAL PART BUILDER
-- =================================================

local function parseColor(val)
    if type(val) == "string" then
        local hex = val:gsub("^#", "")
        if #hex == 6 or #hex == 8 then
            local r = tonumber(hex:sub(1, 2), 16)
            local g = tonumber(hex:sub(3, 4), 16)
            local b = tonumber(hex:sub(5, 6), 16)
            if r and g and b then
                return { r / 255, g / 255, b / 255 }
            end
        end
    elseif type(val) == "table" and #val >= 3 then
        local r, g, b = tonumber(val[1]), tonumber(val[2]), tonumber(val[3])
        if r and g and b then
            if r > 1 or g > 1 or b > 1 then
                return { r / 255, g / 255, b / 255 }
            end
            return { r, g, b }
        end
    end
    return nil
end

local function hashColor(label, minV, spanV)
    local h = 2166136261
    for i = 1, #label do
        h = ((h * 16777619) + label:byte(i)) % 4294967296
    end
    local r = minV + spanV * ((h % 97) / 96)
    local g = minV + spanV * ((math.floor(h / 97) % 97) / 96)
    local b = minV + spanV * ((math.floor(h / 9409) % 97) / 96)
    return {r, g, b}
end

local KNOWN_SOCKET_COLORS = {
    WALL      = {0.14, 0.15, 0.18},
    OPEN      = {0.86, 0.88, 0.90},
    WATER     = {0.16, 0.45, 0.78},
    OCEAN     = {0.12, 0.36, 0.72},
    RIVER     = {0.22, 0.60, 0.90},
    GRASS     = {0.26, 0.62, 0.28},
    FOREST    = {0.14, 0.42, 0.20},
    SAND      = {0.84, 0.76, 0.46},
    SHORE     = {0.72, 0.72, 0.56},
    ROAD      = {0.62, 0.54, 0.42},
    PATH      = {0.68, 0.58, 0.45},
    STONE     = {0.48, 0.50, 0.54},
    ROCK      = {0.44, 0.42, 0.40},
    CLIFF     = {0.38, 0.30, 0.24},
    PIPE      = {0.22, 0.78, 0.68},
    WIRE      = {0.92, 0.72, 0.18},
    CIRCUIT   = {0.20, 0.85, 0.48},
    DOOR      = {0.76, 0.48, 0.22},
    VENT      = {0.58, 0.72, 0.82},
    LAVA      = {0.92, 0.32, 0.12},
}

local function resolveSocketColor(sname, sdef)
    if sdef and sdef.color then return sdef.color end
    local upper = tostring(sname or ""):upper()
    if KNOWN_SOCKET_COLORS[upper] then
        return KNOWN_SOCKET_COLORS[upper]
    end
    for kw, col in pairs(KNOWN_SOCKET_COLORS) do
        if upper:find(kw, 1, true) then return col end
    end
    if sdef and sdef.open then
        return hashColor(sname, 0.45, 0.45)
    end
    return hashColor(sname, 0.24, 0.38)
end

-- Image cache (cleared on reload / export)
local loadedImages = {}

local function clearImageCache()
    loadedImages = {}
end

local function tryLoadImage(path)
    if not path or path == "" then return nil end
    local norm = normalizePath(path)
    local cached = loadedImages[norm]
    if cached ~= nil then
        return cached or nil
    end

    local img = nil
    if love and love.graphics and love.graphics.newImage then
        if love.filesystem and love.filesystem.getInfo and love.filesystem.getInfo(norm) then
            local ok, res = pcall(love.graphics.newImage, norm)
            if ok and res then img = res end
        end
        if not img then
            local f = io.open(path, "rb") or io.open(norm, "rb")
            if f then
                local bytes = f:read("*all")
                f:close()
                if bytes and #bytes > 0 and love.filesystem and love.filesystem.newFileData then
                    local okFd, fd = pcall(love.filesystem.newFileData, bytes, norm)
                    if okFd and fd then
                        local okImg, res = pcall(love.graphics.newImage, fd)
                        if okImg and res then img = res end
                    end
                elseif love.window then
                    -- Headless/mocked test harnesses may mock newImage directly by string path
                    local okImg, res = pcall(love.graphics.newImage, norm)
                    if okImg and res then img = res end
                end
            end
        end
    end

    if img and img.setFilter then
        pcall(img.setFilter, img, "nearest", "nearest")
    end
    loadedImages[norm] = img or false
    return img
end

-- Build a crisp 32x32 RGBA pixel-art ImageData representing any tile's fill & N/E/S/W sockets
local function buildProceduralImageData(tile, sockets)
    if not (love and love.image and love.image.newImageData) then
        return nil
    end
    local sz = PROC_TILE_PX
    local id = love.image.newImageData(sz, sz)
    local base = tile.baseColor

    local colN = resolveSocketColor(tile.sock.N, sockets[tile.sock.N])
    local colE = resolveSocketColor(tile.sock.E, sockets[tile.sock.E])
    local colS = resolveSocketColor(tile.sock.S, sockets[tile.sock.S])
    local colW = resolveSocketColor(tile.sock.W, sockets[tile.sock.W])

    local openCount = (tile.open.N and 1 or 0) + (tile.open.E and 1 or 0)
                    + (tile.open.S and 1 or 0) + (tile.open.W and 1 or 0)

    local function setP(px, py, r, g, b)
        id:setPixel(px, py,
            math.max(0, math.min(1, r)),
            math.max(0, math.min(1, g)),
            math.max(0, math.min(1, b)), 1)
    end

    for py = 0, sz - 1 do
        for px = 0, sz - 1 do
            -- Subtle checker grain so identical tiles still look textured
            local checker = ((px + py) % 4 == 0) and 0.03 or (((px * 3 + py) % 7 == 0) and -0.025 or 0)
            local r = base[1] + checker
            local g = base[2] + checker
            local b = base[3] + checker

            -- Subtle outer bevel border
            if px == 0 or py == 0 or px == sz - 1 or py == sz - 1 then
                r, g, b = r * 0.72, g * 0.72, b * 0.72
            end

            -- Non-WALL closed edges show thin transition strip of their socket color
            if py >= 1 and py <= 3 and not tile.open.N and tile.sock.N ~= "WALL" then
                r, g, b = colN[1], colN[2], colN[3]
            elseif py >= sz - 4 and py <= sz - 2 and not tile.open.S and tile.sock.S ~= "WALL" then
                r, g, b = colS[1], colS[2], colS[3]
            elseif px >= 1 and px <= 3 and not tile.open.W and tile.sock.W ~= "WALL" then
                r, g, b = colW[1], colW[2], colW[3]
            elseif px >= sz - 4 and px <= sz - 2 and not tile.open.E and tile.sock.E ~= "WALL" then
                r, g, b = colE[1], colE[2], colE[3]
            end

            -- Open ports carve bright socket paths cleanly to center hub
            local hasAnyOpen6 = (openCount > 0) or (tile.open and (tile.open.U or tile.open.D))
            if hasAnyOpen6 then
                local inHub = (px >= 11 and px <= 20 and py >= 11 and py <= 20)
                local inN = tile.open.N and (px >= 12 and px <= 19 and py <= 15)
                local inS = tile.open.S and (px >= 12 and px <= 19 and py >= 16)
                local inW = tile.open.W and (py >= 12 and py <= 19 and px <= 15)
                local inE = tile.open.E and (py >= 12 and py <= 19 and px >= 16)

                local chanCol = nil
                if inN then chanCol = colN
                elseif inS then chanCol = colS
                elseif inW then chanCol = colW
                elseif inE then chanCol = colE
                elseif inHub then
                    chanCol = tile.open.N and colN or (tile.open.E and colE or (tile.open.S and colS or (tile.open.W and colW or base)))
                end

                if chanCol then
                    r, g, b = chanCol[1], chanCol[2], chanCol[3]
                    -- Vertical port markers inside the center hub for 2D slice view:
                    -- Outer gold ring = opens UP (U), inner cyan core = opens DOWN (D)
                    if inHub and tile.open then
                        local cxDist = math.max(math.abs(px - 15.5), math.abs(py - 15.5))
                        if tile.open.U and cxDist >= 3.0 and cxDist <= 4.6 then
                            r, g, b = 0.98, 0.72, 0.22
                        elseif tile.open.D and cxDist <= 2.2 then
                            r, g, b = 0.22, 0.82, 0.98
                        end
                    end
                elseif (px >= 10 and px <= 21 and py >= 10 and py <= 21)
                    or (tile.open.N and px >= 10 and px <= 21 and py <= 16)
                    or (tile.open.S and px >= 10 and px <= 21 and py >= 15)
                    or (tile.open.W and py >= 10 and py <= 21 and px <= 16)
                    or (tile.open.E and py >= 10 and py <= 21 and px >= 15) then
                    -- Dark crisp outline around open channels
                    r, g, b = base[1] * 0.45, base[2] * 0.45, base[3] * 0.45
                end
            else
                -- 0-open-port tile emblem (small centered diamond based on category/tile color)
                local dx = math.abs(px - 15.5)
                local dy = math.abs(py - 15.5)
                if dx + dy <= 5.5 then
                    local boost = (dx + dy <= 2.5) and 1.25 or 0.85
                    r, g, b = base[1] * boost + 0.08, base[2] * boost + 0.08, base[3] * boost + 0.08
                end
            end

            setP(px, py, r, g, b)
        end
    end

    return id
end

-- =================================================
-- DYNAMIC RULESET LOADER & VISUAL RESOLVER
-- =================================================

local FALLBACK_RULESET_DATA = {
    name = "fallback_standard_16",
    sockets = {
        WALL = { open = false, compatible = {"WALL"} },
        OPEN = { open = true,  compatible = {"OPEN"} }
    },
    architect = {
        vertical_anchors = {
            { lower = "Special_X_Corridor", upper = "Normal_X_Corridor" }
        }
    },
    tiles = {
        { name = "North_East_Corridor",  ports = { N = "OPEN", E = "OPEN", S = "WALL", W = "WALL" } },
        { name = "South_East_Corridor",  ports = { N = "WALL", E = "OPEN", S = "OPEN", W = "WALL" } },
        { name = "South_West_Corridor",  ports = { N = "WALL", E = "WALL", S = "OPEN", W = "OPEN" } },
        { name = "North_West_Corridor",  ports = { N = "OPEN", E = "WALL", S = "WALL", W = "OPEN" } },
        { name = "North_South_Corridor", ports = { N = "OPEN", E = "WALL", S = "OPEN", W = "WALL" } },
        { name = "West_East_Corridor",   ports = { N = "WALL", E = "OPEN", S = "WALL", W = "OPEN" } },
        { name = "North_T_Corridor",     ports = { N = "OPEN", E = "OPEN", S = "WALL", W = "OPEN" } },
        { name = "East_T_Corridor",      ports = { N = "OPEN", E = "OPEN", S = "OPEN", W = "WALL" } },
        { name = "South_T_Corridor",     ports = { N = "WALL", E = "OPEN", S = "OPEN", W = "OPEN" } },
        { name = "West_T_Corridor",      ports = { N = "OPEN", E = "WALL", S = "OPEN", W = "OPEN" } },
        { name = "Normal_X_Corridor",    ports = { N = "OPEN", E = "OPEN", S = "OPEN", W = "OPEN" } },
        { name = "Special_X_Corridor",   ports = { N = "OPEN", E = "OPEN", S = "OPEN", W = "OPEN" } },
        { name = "North_DeadEnd",        ports = { N = "OPEN", E = "WALL", S = "WALL", W = "WALL" } },
        { name = "East_DeadEnd",         ports = { N = "WALL", E = "OPEN", S = "WALL", W = "WALL" } },
        { name = "South_DeadEnd",        ports = { N = "WALL", E = "WALL", S = "OPEN", W = "WALL" } },
        { name = "West_DeadEnd",         ports = { N = "WALL", E = "WALL", S = "WALL", W = "OPEN" } }
    }
}

-- Derive legacy corridor shape key ("X","T","L","I","D") + rotation for mock corridor fallback
local function resolveCorridorShape(openN, openE, openS, openW)
    local count = (openN and 1 or 0) + (openE and 1 or 0) + (openS and 1 or 0) + (openW and 1 or 0)
    if count == 4 then
        return "X", 0
    elseif count == 3 then
        if not openS then return "T", 0
        elseif not openW then return "T", math.pi / 2
        elseif not openN then return "T", math.pi
        else return "T", 3 * math.pi / 2 end
    elseif count == 2 then
        if openN and openS then return "I", 0
        elseif openE and openW then return "I", math.pi / 2
        elseif openN and openE then return "L", 0
        elseif openS and openE then return "L", math.pi / 2
        elseif openS and openW then return "L", math.pi
        else return "L", 3 * math.pi / 2 end
    elseif count == 1 then
        if openN then return "D", 0
        elseif openE then return "D", math.pi / 2
        elseif openS then return "D", math.pi
        else return "D", 3 * math.pi / 2 end
    end
    return nil, 0
end

local function computeSocketTint(openSockets, sockets)
    for _, sname in ipairs(openSockets) do
        if sname ~= "OPEN" then
            return resolveSocketColor(sname, sockets[sname])
        end
    end
    return {1, 1, 1}
end

local function pickTileBaseColor(tdef, categoryColors, sockets, sockN, sockE, sockS, sockW, openCount)
    local explicit = parseColor(tdef.color)
    if explicit then return explicit end
    if tdef.category and categoryColors[tdef.category] then
        return categoryColors[tdef.category]
    end
    if openCount == 0 then
        -- Non-corridor solid tile: derive clean fill from dominant port socket
        local votes = {}
        local bestSock, bestCnt = sockN, 0
        for _, s in ipairs({sockN, sockE, sockS, sockW}) do
            votes[s] = (votes[s] or 0) + 1
            if votes[s] > bestCnt then
                bestCnt = votes[s]
                bestSock = s
            end
        end
        if bestSock ~= "WALL" then
            return resolveSocketColor(bestSock, sockets[bestSock])
        end
        return hashColor(tdef.name or bestSock, 0.25, 0.45)
    end
    return {0.20, 0.22, 0.25}
end

local function collectUniqueDirs(...)
    local out, seen = {}, {}
    for i = 1, select("#", ...) do
        local d = select(i, ...)
        if d and d ~= "" then
            local norm = normalizePath(d):gsub("/+$", "")
            if norm ~= "" and not seen[norm] then
                seen[norm] = true
                out[#out + 1] = norm
            end
        end
    end
    return out
end

local function resolveTileSprite(tile, rulesetMeta)
    local dirs = rulesetMeta.searchDirs
    local nameCandidates = { tile.name, tile.name:lower() }

    -- 1. Explicit tile sprite/image field
    if tile.explicitSprite and tile.explicitSprite ~= "" then
        local sp = normalizePath(tile.explicitSprite)
        local tries = {}
        if rulesetMeta.imageDir ~= "" then
            tries[#tries + 1] = joinPath(rulesetMeta.imageDir, sp)
        end
        if rulesetMeta.jsonDir ~= "" then
            tries[#tries + 1] = joinPath(rulesetMeta.jsonDir, sp)
        end
        tries[#tries + 1] = sp
        for _, p in ipairs(tries) do
            local img = tryLoadImage(p)
            if img then
                return img, tile.extraRotRad, tile.explicitTint or {1, 1, 1}, "png", p
            end
        end
    end

    -- 2. Auto-discovered per-tile PNG (<tile.name>.png)
    for _, dir in ipairs(dirs) do
        for _, nm in ipairs(nameCandidates) do
            local p = (dir ~= "") and (dir .. "/" .. nm .. ".png") or (nm .. ".png")
            local img = tryLoadImage(p)
            if img then
                return img, tile.extraRotRad, tile.explicitTint or {1, 1, 1}, "png", p
            end
        end
    end

    -- 3. Auto-discovered category PNG (<category>.png)
    if tile.category and tile.category ~= "" then
        for _, dir in ipairs(dirs) do
            if dir ~= "tile_mock" then
                for _, nm in ipairs({tile.category, tile.category:lower()}) do
                    local p = (dir ~= "") and (dir .. "/" .. nm .. ".png") or (nm .. ".png")
                    local img = tryLoadImage(p)
                    if img then
                        return img, tile.extraRotRad, tile.explicitTint or {1, 1, 1}, "png", p
                    end
                end
            end
        end
    end

    -- 4. Topology corridor fallback (X/T/L/I/D.png) ONLY when tile has 1..4 open ports
    if rulesetMeta.tileMockFallback and tile.topoKey then
        local mockSearch = collectUniqueDirs(
            rulesetMeta.imageDir,
            rulesetMeta.jsonDir ~= "" and joinPath(rulesetMeta.jsonDir, "images") or "",
            rulesetMeta.jsonDir,
            "tile_mock"
        )
        for _, dir in ipairs(mockSearch) do
            local p = (dir ~= "") and (dir .. "/" .. tile.topoKey .. ".png") or (tile.topoKey .. ".png")
            local img = tryLoadImage(p)
            if img then
                return img, tile.topoRot + tile.extraRotRad, tile.explicitTint or tile.socketTint, "mock", p
            end
        end
    end

    -- 5. Procedural 32x32 tile-part image (in-memory ImageData -> Image, zero files required)
    local id = buildProceduralImageData(tile, rulesetMeta.sockets)
    tile.procImageData = id
    if id and love and love.graphics and love.graphics.newImage then
        local ok, img = pcall(love.graphics.newImage, id)
        if ok and img then
            if img.setFilter then pcall(img.setFilter, img, "nearest", "nearest") end
            return img, tile.extraRotRad, tile.explicitTint or {1, 1, 1}, "proc", nil
        end
    end

    return nil, tile.extraRotRad, tile.explicitTint or tile.baseColor, "rect", nil
end

local function buildRuleset(data, sourcePath)
    local jsonDir = (sourcePath ~= "built-in fallback") and pathDir(sourcePath) or ""
    local stem = (sourcePath ~= "built-in fallback") and pathStem(sourcePath) or "tileset"
    local rname = data.name or stem
    local slug = sanitizeSlug(rname)

    local sockets = {}
    local openSocketCount = 0
    for sname, sdef in pairs(data.sockets or {}) do
        local comp = {}
        for _, target in ipairs(sdef.compatible or {}) do
            comp[target] = true
        end
        local isOpen = (sdef.open == true)
        if isOpen then openSocketCount = openSocketCount + 1 end
        sockets[sname] = {
            open = isOpen,
            compatible = comp,
            color = parseColor(sdef.color)
        }
    end

    local categoryColors = {}
    for _, cdef in ipairs(data.categories or {}) do
        if cdef.name and cdef.color then
            categoryColors[cdef.name] = parseColor(cdef.color)
        end
    end

    local lowerAnchors = {}
    local upperAnchorPairs = {}
    local arch = data.vertical_rules or data.architect or {}
    local defUpSock = arch.default_up_socket or "WALL"
    local defDnSock = arch.default_down_socket or "WALL"
    local rawAnchors = arch.area_quotas or arch.connectors or arch.vertical_anchors or {}
    for _, anchor in ipairs(rawAnchors) do
        local loName = anchor.tile or anchor.lower
        local upName = anchor.upper
        if loName then
            lowerAnchors[loName] = true
            if upName then
                upperAnchorPairs[upName] = upperAnchorPairs[upName] or {}
                upperAnchorPairs[upName][loName] = true
            end
        end
    end

    -- Resolve image search directory hints
    local rawImgDir = data.image_dir or data.sprite_dir or data.assets_dir or data.images_dir or ""
    local resolvedImgDir = ""
    if rawImgDir ~= "" then
        resolvedImgDir = (jsonDir ~= "" and not rawImgDir:match("^/") and not rawImgDir:match("^%a:"))
            and joinPath(jsonDir, rawImgDir) or normalizePath(rawImgDir)
    end

    local defaultExportDir = resolvedImgDir ~= "" and resolvedImgDir
        or (jsonDir:find("^" .. TILESETS_ROOT_DIR .. "/") and joinPath(jsonDir, "images")
        or joinPath(TILESETS_ROOT_DIR, slug, "images"))

    local searchDirs = collectUniqueDirs(
        resolvedImgDir,
        jsonDir ~= "" and joinPath(jsonDir, "images") or "",
        jsonDir ~= "" and joinPath(jsonDir, "sprites") or "",
        jsonDir ~= "" and joinPath(jsonDir, "tiles") or "",
        jsonDir,
        joinPath(TILESETS_ROOT_DIR, slug, "images"),
        joinPath(TILESETS_ROOT_DIR, slug),
        joinPath(TILESETS_ROOT_DIR, stem, "images"),
        joinPath(TILESETS_ROOT_DIR, stem),
        "tile_mock"
    )

    local rulesetMeta = {
        sockets = sockets,
        jsonDir = jsonDir,
        imageDir = resolvedImgDir,
        searchDirs = searchDirs,
        tileMockFallback = (data.tile_mock_fallback ~= false)
    }

    local rawTiles = data.tiles or {}
    local numTiles = #rawTiles
    local containerBits = (numTiles <= 8 and 8) or (numTiles <= 16 and 16) or (numTiles <= 32 and 32) or 64
    local pow2 = buildPow2Strings(math.max(numTiles, 64))

    local tilesOrdered = {}
    local tileByMaskStr = {}
    local sourceCounts = { png = 0, mock = 0, proc = 0, rect = 0 }

    for i = 1, numTiles do
        local t = rawTiles[i]
        local id = i - 1
        local maskStr = pow2[id]
        local ports = t.ports or {}

        local sockN = ports.N or "WALL"
        local sockE = ports.E or "WALL"
        local sockS = ports.S or "WALL"
        local sockW = ports.W or "WALL"
        local sockU = ports.U or defUpSock
        local sockD = ports.D or defDnSock

        local openN = sockets[sockN] and sockets[sockN].open or false
        local openE = sockets[sockE] and sockets[sockE].open or false
        local openS = sockets[sockS] and sockets[sockS].open or false
        local openW = sockets[sockW] and sockets[sockW].open or false
        local openU = sockets[sockU] and sockets[sockU].open or false
        local openD = sockets[sockD] and sockets[sockD].open or false
        local openCount = (openN and 1 or 0) + (openE and 1 or 0) + (openS and 1 or 0) + (openW and 1 or 0)

        local topoKey, topoRot = resolveCorridorShape(openN, openE, openS, openW)
        local openSocketNames = {}
        if openN then openSocketNames[#openSocketNames + 1] = sockN end
        if openE then openSocketNames[#openSocketNames + 1] = sockE end
        if openS then openSocketNames[#openSocketNames + 1] = sockS end
        if openW then openSocketNames[#openSocketNames + 1] = sockW end

        local extraRotRad = tonumber(t.rot_rad) or (math.rad(tonumber(t.rotation or t.rot or 0) or 0))
        local tileName = t.name or ("Tile_" .. tostring(id))

        local tileEntry = {
            id = id,
            name = tileName,
            category = t.category or "",
            maskStr = maskStr,
            explicitSprite = t.sprite or t.image,
            explicitTint = parseColor(t.tint),
            flipX = (t.flip_x == true),
            flipY = (t.flip_y == true),
            topoKey = topoKey,
            topoRot = topoRot,
            extraRotRad = extraRotRad,
            socketTint = computeSocketTint(openSocketNames, sockets),
            baseColor = pickTileBaseColor(t, categoryColors, sockets, sockN, sockE, sockS, sockW, openCount),
            isLowerAnchor = (lowerAnchors[tileName] == true),
            openCount = openCount,
            open = { N = openN, E = openE, S = openS, W = openW, U = openU, D = openD },
            sock = { N = sockN, E = sockE, S = sockS, W = sockW, U = sockU, D = sockD }
        }

        local img, rRad, drawTint, sourceKind, imgPath = resolveTileSprite(tileEntry, rulesetMeta)
        tileEntry.img = img
        tileEntry.r = rRad
        tileEntry.tint = drawTint
        tileEntry.sourceKind = sourceKind
        tileEntry.imgPath = imgPath
        sourceCounts[sourceKind] = (sourceCounts[sourceKind] or 0) + 1

        tileByMaskStr[maskStr] = tileEntry
        tilesOrdered[i] = tileEntry
    end

    -- Auto-link lowerAnchors to compatible upper receiver tiles via U/D sockets when upper was omitted
    for _, loTile in ipairs(tilesOrdered) do
        if loTile.isLowerAnchor then
            local uSock = sockets[loTile.sock.U]
            if uSock and uSock.compatible then
                for _, hiTile in ipairs(tilesOrdered) do
                    if uSock.compatible[hiTile.sock.D] then
                        upperAnchorPairs[hiTile.name] = upperAnchorPairs[hiTile.name] or {}
                        upperAnchorPairs[hiTile.name][loTile.name] = true
                    end
                end
            end
        end
    end

    local defaultTraversal = (data.is_maze ~= false and openSocketCount > 0)
    if data.traversal_overlay ~= nil then
        defaultTraversal = (data.traversal_overlay == true)
    end

    return {
        name = rname,
        slug = slug,
        sourcePath = sourcePath,
        jsonDir = jsonDir,
        stem = stem,
        imageDir = resolvedImgDir,
        exportDir = defaultExportDir,
        mazeFileHint = data.maze_file,
        maskFileHint = data.mask_file,
        defaultTraversal = defaultTraversal,
        openSocketCount = openSocketCount,
        numTiles = numTiles,
        containerBits = containerBits,
        sockets = sockets,
        upperAnchorPairs = upperAnchorPairs,
        tilesOrdered = tilesOrdered,
        tileByMaskStr = tileByMaskStr,
        sourceCounts = sourceCounts
    }
end

-- =================================================
-- MULTI-TILESET DISCOVERY & ENGINE STATE
-- =================================================
local discoveredTilesets = {}
local activeTilesetIdx = 1
local ruleset = nil

local grid = {}
local maskGrid = {}
local reachableGrid = {}
local width, length, height = 0, 0, 0
local uncollapsedCount = 0
local activeMazePath = "maze.txt"
local tileSize = 32

-- View / UI state
local currentLayer = 1
local showMask = false
local showTraversal = true
local showPalette = false
local paletteZoom = 1.0
local paletteScrollY = 0
local statusMessage = ""
local statusTimer = 0

-- Camera (2D & 3D Orbit)
local camX, camY = 0, 0
local camZoom = 1.0

local view3D = true
local yaw3D = 0.68
local pitch3D = 0.62
local zoom3D = 1.0
local pan3DX, pan3DY = 0, 0
local layerGap3D = 2.15
local cutawayMode3D = 1 -- 1 = All Layers, 2 = Cutaway <= currentLayer, 3 = Highlight currentLayer
local showAir3D = false

local function setStatus(msg)
    statusMessage = msg
    statusTimer = 5.0
    print(msg)
end

local function isTilesetJsonContent(raw)
    if not raw then return false end
    local ok, parsed = pcall(decodeJSON, raw)
    return ok and type(parsed) == "table" and type(parsed.tiles) == "table" and #parsed.tiles > 0
end

local function fileExistsAny(path)
    if not path or path == "" then return false end
    local norm = normalizePath(path)
    if love and love.filesystem and love.filesystem.getInfo and love.filesystem.getInfo(norm) then
        return true
    end
    local f = io.open(path, "rb") or io.open(norm, "rb")
    if f then f:close(); return true end
    return false
end

function discoverTilesets()
    local list = {}
    local seen = {}

    local function addIfValid(path, requireTilesKey)
        if not path or path == "" then return end
        local norm = normalizePath(path)
        if seen[norm] then return end
        if not fileExistsAny(path) then return end
        if requireTilesKey then
            local raw = readTextFile(path)
            if not isTilesetJsonContent(raw) then return end
        end
        seen[norm] = true
        list[#list + 1] = norm
    end

    local envPath = os.getenv("CGSME_TILESET")
    if envPath and envPath ~= "" then
        local normEnv = normalizePath(envPath)
        seen[normEnv] = true
        list[#list + 1] = normEnv
    end
    addIfValid(DEFAULT_TILESET_FILE, false)

    -- Discover any tilesets inside tilesets/*.json and tilesets/<pack>/*.json
    if love and love.filesystem and love.filesystem.getInfo and love.filesystem.getDirectoryItems then
        if love.filesystem.setSymlinksEnabled then
            pcall(love.filesystem.setSymlinksEnabled, true)
        end
        local info = love.filesystem.getInfo(TILESETS_ROOT_DIR, "directory")
        if info then
            local entries = love.filesystem.getDirectoryItems(TILESETS_ROOT_DIR)
            table.sort(entries)
            for _, entry in ipairs(entries) do
                local full = joinPath(TILESETS_ROOT_DIR, entry)
                if entry:match("%.json$") then
                    addIfValid(full, true)
                else
                    local subInfo = love.filesystem.getInfo(full, "directory")
                    if subInfo then
                        local subEntries = love.filesystem.getDirectoryItems(full)
                        table.sort(subEntries)
                        -- Prefer tileset.json inside pack folder first
                        for _, sub in ipairs(subEntries) do
                            if sub == "tileset.json" then
                                addIfValid(joinPath(full, sub), true)
                            end
                        end
                        for _, sub in ipairs(subEntries) do
                            if sub:match("%.json$") and sub ~= "tileset.json" then
                                addIfValid(joinPath(full, sub), true)
                            end
                        end
                    end
                end
            end
        end
    end

    if #list == 0 then
        list[1] = DEFAULT_TILESET_FILE
    end

    discoveredTilesets = list
    if activeTilesetIdx > #discoveredTilesets then
        activeTilesetIdx = 1
    end
end

function loadRuleset(explicitPath)
    local path = explicitPath or (discoveredTilesets[activeTilesetIdx]) or os.getenv("CGSME_TILESET") or DEFAULT_TILESET_FILE
    local raw = readTextFile(path)

    if not raw and path ~= DEFAULT_TILESET_FILE and not explicitPath then
        print("WARNING: Could not read tileset '" .. path .. "', trying " .. DEFAULT_TILESET_FILE)
        path = DEFAULT_TILESET_FILE
        raw = readTextFile(path)
    end

    if raw then
        local ok, parsed = pcall(decodeJSON, raw)
        if ok and type(parsed) == "table" then
            ruleset = buildRuleset(parsed, path)
            showTraversal = ruleset.defaultTraversal
            print(string.format("Loaded ruleset '%s' (%d tiles, %d-bit, sprites png=%d mock=%d proc=%d) from %s",
                ruleset.name, ruleset.numTiles, ruleset.containerBits,
                ruleset.sourceCounts.png, ruleset.sourceCounts.mock, ruleset.sourceCounts.proc, path))
            return
        else
            print("WARNING: Failed to parse JSON from '" .. path .. "': " .. tostring(parsed))
        end
    else
        print("WARNING: '" .. path .. "' not found, using built-in 16-tile fallback.")
    end

    ruleset = buildRuleset(FALLBACK_RULESET_DATA, "built-in fallback")
    showTraversal = ruleset.defaultTraversal
end

-- =================================================
-- PNG PART EXPORTER (Generates starter PNG files)
-- =================================================

function exportTilesetImages(overwrite)
    if not ruleset then return 0, 0 end
    if not (love and love.image and love.image.newImageData) then
        setStatus("PNG export unavailable (love.image module inactive).")
        return 0, 0
    end

    local srcRoot = (love and love.filesystem and love.filesystem.getSource and love.filesystem.getSource()) or "."
    local relDir = ruleset.exportDir
    local outDir = (relDir:match("^/") or relDir:match("^%a:")) and relDir or joinPath(srcRoot, relDir)
    makeDirRecursive(outDir)

    local written, skipped = 0, 0
    for _, tile in ipairs(ruleset.tilesOrdered) do
        local outFile = joinPath(outDir, tile.name .. ".png")
        local exists = false
        if not overwrite then
            local check = io.open(outFile, "rb")
            if check then check:close(); exists = true end
        end

        if exists then
            skipped = skipped + 1
        else
            local id = tile.procImageData or buildProceduralImageData(tile, ruleset.sockets)
            if id then
                local fd = id:encode("png")
                local bytes = fd:getString()
                local f = io.open(outFile, "wb")
                if f then
                    f:write(bytes)
                    f:close()
                    written = written + 1
                end
            end
        end
    end

    clearImageCache()
    loadRuleset()
    setStatus(string.format("Exported %d PNG tile parts to '%s' (%d skipped).", written, relDir, skipped))
    return written, skipped
end

-- =================================================
-- 3D 6-DIRECTIONAL FLOOD FILL TRAVERSAL LOGIC
-- =================================================

local totalValid3D = 0
local totalReachable3D = 0

local function isValidTile(tile)
    return type(tile) == "table" and not tile.uncollapsed
end

local function socketsCompatible(sock1, sock2)
    if not sock1 or not sock2 or not ruleset then return false end
    local sdef = ruleset.sockets[sock1]
    return sdef ~= nil and (sdef.compatible[sock2] == true)
end

local function canTraverse3D(x1, y1, z1, x2, y2, z2)
    local layer1 = grid[z1]
    local layer2 = grid[z2]
    if not layer1 or not layer2 or not layer1[y1] or not layer2[y2] then return false end
    local t1 = layer1[y1][x1]
    local t2 = layer2[y2][x2]
    if not isValidTile(t1) or not isValidTile(t2) then return false end

    local dx = x2 - x1
    local dy = y2 - y1
    local dz = z2 - z1

    if dz == 0 then
        if dy == -1 and dx == 0 then
            return t1.open.N and t2.open.S and socketsCompatible(t1.sock.N, t2.sock.S)
        elseif dy == 1 and dx == 0 then
            return t1.open.S and t2.open.N and socketsCompatible(t1.sock.S, t2.sock.N)
        elseif dx == 1 and dy == 0 then
            return t1.open.E and t2.open.W and socketsCompatible(t1.sock.E, t2.sock.W)
        elseif dx == -1 and dy == 0 then
            return t1.open.W and t2.open.E and socketsCompatible(t1.sock.W, t2.sock.E)
        end
    elseif dx == 0 and dy == 0 then
        local upperPairs = ruleset and ruleset.upperAnchorPairs or {}
        if dz == 1 then
            if t1.open.U and t2.open.D and socketsCompatible(t1.sock.U, t2.sock.D) then
                return true
            end
            if t1.isLowerAnchor and upperPairs[t2.name] and upperPairs[t2.name][t1.name] then
                return true
            end
        elseif dz == -1 then
            if t1.open.D and t2.open.U and socketsCompatible(t1.sock.D, t2.sock.U) then
                return true
            end
            if t2.isLowerAnchor and upperPairs[t1.name] and upperPairs[t1.name][t2.name] then
                return true
            end
        end
    end

    return false
end

local function rulesetHasVerticalTraversal()
    if not ruleset then return false end
    if next(ruleset.upperAnchorPairs or {}) ~= nil then
        return true
    end
    for _, t in ipairs(ruleset.tilesOrdered or {}) do
        if t.open and (t.open.U or t.open.D) then
            return true
        end
    end
    return false
end

function runFloodFill()
    reachableGrid = {}
    for z = 1, height do
        reachableGrid[z] = {}
        for y = 1, length do
            reachableGrid[z][y] = {}
        end
    end

    local hasVert = (height > 1) and rulesetHasVerticalTraversal()
    local dx6 = {  0,  1,  0, -1,  0,  0 }
    local dy6 = { -1,  0,  1,  0,  0,  0 }
    local dz6 = {  0,  0,  0,  0,  1, -1 }
    local maxDirs = hasVert and 6 or 4

    local function bfsFrom(sx, sy, sz)
        local qx = { sx }
        local qy = { sy }
        local qz = { sz }
        reachableGrid[sz][sy][sx] = true
        local head, tail = 1, 1

        while head <= tail do
            local cx, cy, cz = qx[head], qy[head], qz[head]
            head = head + 1
            for d = 1, maxDirs do
                local nx = cx + dx6[d]
                local ny = cy + dy6[d]
                local nz = cz + dz6[d]
                if nx >= 1 and nx <= width and ny >= 1 and ny <= length and nz >= 1 and nz <= height then
                    if not reachableGrid[nz][ny][nx] and canTraverse3D(cx, cy, cz, nx, ny, nz) then
                        reachableGrid[nz][ny][nx] = true
                        tail = tail + 1
                        qx[tail] = nx
                        qy[tail] = ny
                        qz[tail] = nz
                    end
                end
            end
        end
    end

    if hasVert then
        -- True 3D flood fill from a single starting tile across the entire 3D volume (N,E,S,W,U,D)
        local seeded = false
        for z = 1, height do
            if grid[z] and not seeded then
                for y = 1, length do
                    local row = grid[z][y]
                    if row then
                        for x = 1, width do
                            if isValidTile(row[x]) then
                                bfsFrom(x, y, z)
                                seeded = true
                                break
                            end
                        end
                    end
                    if seeded then break end
                end
            end
        end
    else
        -- 2D per-layer flood fill for rulesets without vertical traversal sockets
        for z = 1, height do
            if grid[z] then
                local seeded = false
                for y = 1, length do
                    local row = grid[z][y]
                    if row then
                        for x = 1, width do
                            if isValidTile(row[x]) then
                                bfsFrom(x, y, z)
                                seeded = true
                                break
                            end
                        end
                    end
                    if seeded then break end
                end
            end
        end
    end

    totalValid3D = 0
    totalReachable3D = 0
    for z = 1, height do
        local layer = grid[z]
        local rLayer = reachableGrid[z]
        if layer then
            for y = 1, length do
                local row = layer[y]
                local rRow = rLayer and rLayer[y]
                if row then
                    for x = 1, width do
                        if isValidTile(row[x]) then
                            totalValid3D = totalValid3D + 1
                            if rRow and rRow[x] then
                                totalReachable3D = totalReachable3D + 1
                            end
                        end
                    end
                end
            end
        end
    end

    print(string.format("3D Flood Fill Complete: %d/%d tiles reachable across %d layers (%.2f%%)",
                        totalReachable3D, totalValid3D, height,
                        totalValid3D > 0 and (totalReachable3D / totalValid3D * 100) or 0))
end

-- =================================================
-- MAZE & MASK LOADERS (Per-Tileset Aware)
-- =================================================

local function resolveDataFile(hint, perPackName, fallbackName)
    local candidates = {}
    if hint and hint ~= "" then
        if ruleset and ruleset.jsonDir ~= "" and not hint:match("^/") and not hint:match("^%a:") then
            candidates[#candidates + 1] = joinPath(ruleset.jsonDir, hint)
        end
        candidates[#candidates + 1] = normalizePath(hint)
    end
    if ruleset and ruleset.jsonDir ~= "" and ruleset.jsonDir ~= "." then
        candidates[#candidates + 1] = joinPath(ruleset.jsonDir, perPackName)
    end
    if ruleset then
        candidates[#candidates + 1] = joinPath("mazes", ruleset.slug .. "_" .. perPackName)
        candidates[#candidates + 1] = joinPath("mazes", ruleset.stem .. ".txt")
    end
    candidates[#candidates + 1] = fallbackName

    for _, p in ipairs(candidates) do
        local content = readTextFile(p)
        if content then return content, p end
    end
    return nil, fallbackName
end

function loadMaze3D()
    grid = {}
    width, length, height = 0, 0, 0
    uncollapsedCount = 0

    local contents, usedPath = resolveDataFile(
        ruleset and ruleset.mazeFileHint or nil,
        "maze.txt",
        "maze.txt"
    )
    activeMazePath = usedPath
    if not contents then return end

    local byMask = ruleset and ruleset.tileByMaskStr or {}
    local isHeader = true
    local z, y = 1, 1

    for line in contents:gmatch("[^\r\n]+") do
        if isHeader then
            isHeader = false
            local wStr, lStr, hStr = line:match("^%s*([^,]+),([^,]+),([^,]+)")
            width = tonumber(wStr) or 0
            length = tonumber(lStr) or 0
            height = tonumber(hStr) or 1
            for layer = 1, height do
                grid[layer] = {}
            end
        else
            if z <= height then
                local row = {}
                local x = 1
                for val in line:gmatch("([^,]+)") do
                    val = val:match("^%s*(.-)%s*$")
                    if val ~= "" and val ~= "0" then
                        local tile = byMask[val]
                        if not tile then
                            uncollapsedCount = uncollapsedCount + 1
                            row[x] = UNCOLLAPSED_TILE
                        else
                            row[x] = tile
                        end
                    end
                    x = x + 1
                end
                grid[z][y] = row
                y = y + 1
                if y > length then
                    y = 1
                    z = z + 1
                end
            end
        end
    end

    if currentLayer > height and height > 0 then
        currentLayer = height
    elseif currentLayer < 1 then
        currentLayer = 1
    end
end

function loadMask()
    maskGrid = {}
    local contents = resolveDataFile(
        ruleset and ruleset.maskFileHint or nil,
        "debug_mask.txt",
        "debug_mask.txt"
    )
    if not contents then return end

    local isHeader = true
    local y = 1
    for line in contents:gmatch("[^\r\n]+") do
        if isHeader then
            isHeader = false
        else
            if y > length then break end
            local row = {}
            local x = 1
            for val in line:gmatch("([^,]+)") do
                row[x] = (tonumber(val) == 1)
                x = x + 1
            end
            maskGrid[y] = row
            y = y + 1
        end
    end
    print("Loaded Binary Mask Overlay")
end

-- =================================================
-- PALETTE / PARTS VIEW RENDERER ('P' Key)
-- =================================================

local function drawTileVisual(tile, drawX, drawY, size)
    if tile.img then
        local tint = tile.tint or {1, 1, 1}
        love.graphics.setColor(tint[1], tint[2], tint[3])
        local iw, ih = tile.img:getWidth(), tile.img:getHeight()
        local sx = (size / iw) * (tile.flipX and -1 or 1)
        local sy = (size / ih) * (tile.flipY and -1 or 1)
        love.graphics.draw(tile.img, drawX + size / 2, drawY + size / 2,
                           tile.r or 0, sx, sy, iw / 2, ih / 2)
    else
        local bc = tile.baseColor or SOLID_TILE_COLOR
        love.graphics.setColor(bc[1], bc[2], bc[3])
        love.graphics.rectangle("fill", drawX, drawY, size, size)
    end
end

local function drawPaletteView()
    if not ruleset then return end
    local winW = love.graphics.getWidth()
    local cardW = math.floor(195 * paletteZoom)
    local cardH = math.floor(96 * paletteZoom)
    local pad = 12
    local topY = 185
    local cols = math.max(1, math.floor((winW - pad) / (cardW + pad)))
    local previewSize = math.floor(56 * paletteZoom)

    love.graphics.push()
    love.graphics.translate(0, -paletteScrollY)

    for idx, tile in ipairs(ruleset.tilesOrdered) do
        local col = (idx - 1) % cols
        local row = math.floor((idx - 1) / cols)
        local x = pad + col * (cardW + pad)
        local y = topY + row * (cardH + pad)

        love.graphics.setColor(0.11, 0.12, 0.15)
        love.graphics.rectangle("fill", x, y, cardW, cardH)
        love.graphics.setColor(0.28, 0.30, 0.36)
        love.graphics.rectangle("line", x, y, cardW, cardH)

        drawTileVisual(tile, x + 8, y + (cardH - previewSize) / 2, previewSize)

        local tx = x + previewSize + 16
        love.graphics.setColor(1, 1, 1)
        love.graphics.print(string.format("#%d %s", tile.id, tile.name:sub(1, 18)), tx, y + 8)
        love.graphics.setColor(0.70, 0.78, 0.88)
        love.graphics.print(string.format("cat:%s [%s]", tile.category ~= "" and tile.category or "-", tile.sourceKind), tx, y + 26)
        love.graphics.setColor(0.65, 0.90, 0.65)
        love.graphics.print(string.format("N:%s E:%s", tile.sock.N:sub(1, 6), tile.sock.E:sub(1, 6)), tx, y + 46)
        love.graphics.print(string.format("S:%s W:%s", tile.sock.S:sub(1, 6), tile.sock.W:sub(1, 6)), tx, y + 64)
    end

    love.graphics.pop()
end

-- =================================================
-- FULL 3D INTERACTIVE ORBIT RENDERER ('V' / '3' Key)
-- =================================================

local function isAirSkyTile(tile)
    if not tile then return true end
    local cat = (tile.category or ""):upper()
    if cat:find("SKY_VOID", 1, true) or cat == "AIR" or cat == "VOID" then
        return true
    end
    if tile.openCount == 0 and not tile.open.U and not tile.open.D then
        local sN = (tile.sock.N or ""):upper()
        if sN == "AIR" or sN == "SKY" then
            return true
        end
    end
    return false
end

local function draw3DView()
    if width <= 0 or length <= 0 or height <= 0 then return end

    local winW, winH = love.graphics.getWidth(), love.graphics.getHeight()
    local cxScreen = winW * 0.5 + pan3DX
    local cyScreen = winH * 0.54 + pan3DY

    local maxDim = math.max(width, length, height * layerGap3D)
    local baseScale = (math.min(winW, winH) * 0.62 / math.max(4, maxDim)) * zoom3D

    local cosY = math.cos(yaw3D)
    local sinY = math.sin(yaw3D)
    local cosP = math.cos(pitch3D)
    local sinP = math.sin(pitch3D)

    local midX = (width + 1) * 0.5
    local midY = (length + 1) * 0.5
    local midZ = ((height + 1) * 0.5 - 1) * layerGap3D

    local function project3D(wx, wy, wz)
        local dx = wx - midX
        local dy = wy - midY
        local dz = wz - midZ
        local rx = dx * cosY - dy * sinY
        local ry = dx * sinY + dy * cosY
        local sx = cxScreen + rx * baseScale
        local sy = cyScreen + (ry * sinP - dz * cosP) * baseScale
        return sx, sy
    end

    local function drawQuad3D(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a)
        local sx1, sy1 = project3D(x1, y1, z1)
        local sx2, sy2 = project3D(x2, y2, z2)
        local sx3, sy3 = project3D(x3, y3, z3)
        local sx4, sy4 = project3D(x4, y4, z4)
        love.graphics.setColor(r, g, b, a or 1)
        love.graphics.polygon("fill", sx1, sy1, sx2, sy2, sx3, sy3, sx4, sy4)
        love.graphics.setColor(r * 0.42, g * 0.42, b * 0.45, (a or 1) * 0.72)
        love.graphics.polygon("line", sx1, sy1, sx2, sy2, sx3, sy3, sx4, sy4)
    end

    local function drawTri3D(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a)
        local sx1, sy1 = project3D(x1, y1, z1)
        local sx2, sy2 = project3D(x2, y2, z2)
        local sx3, sy3 = project3D(x3, y3, z3)
        love.graphics.setColor(r, g, b, a or 1)
        love.graphics.polygon("fill", sx1, sy1, sx2, sy2, sx3, sy3)
        love.graphics.setColor(r * 0.45, g * 0.45, b * 0.48, (a or 1) * 0.75)
        love.graphics.polygon("line", sx1, sy1, sx2, sy2, sx3, sy3)
    end

    -- Draw a 3D axis-aligned box [x0..x1] x [y0..y1] x [z0..z1] with camera-culled side faces + top face
    local function drawBox3D(x0, y0, z0, x1, y1, z1, r, g, b, a)
        -- North (-Y) or South (+Y) visible side face
        if cosY >= 0 then
            -- Camera looks toward -Y, so South (+Y = y1) face faces camera
            drawQuad3D(x0, y1, z0, x1, y1, z0, x1, y1, z1, x0, y1, z1, r * 0.76, g * 0.76, b * 0.80, a)
        else
            -- North (-Y = y0) face faces camera
            drawQuad3D(x1, y0, z0, x0, y0, z0, x0, y0, z1, x1, y0, z1, r * 0.70, g * 0.70, b * 0.74, a)
        end

        -- East (+X) or West (-X) visible side face
        if sinY >= 0 then
            -- East (+X = x1) face faces camera
            drawQuad3D(x1, y0, z0, x1, y1, z0, x1, y1, z1, x1, y0, z1, r * 0.62, g * 0.62, b * 0.66, a)
        else
            -- West (-X = x0) face faces camera
            drawQuad3D(x0, y1, z0, x0, y0, z0, x0, y0, z1, x0, y1, z1, r * 0.84, g * 0.84, b * 0.88, a)
        end

        -- Top (+Z = z1) face
        drawQuad3D(x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1, r, g, b, a)
    end

    -- Draw a 3D pyramid roof from base [x0..x1] x [y0..y1] at z0 to apex (mx, my, z1)
    local function drawPyramid3D(x0, y0, z0, x1, y1, z1, r, g, b, a)
        local mx = (x0 + x1) * 0.5
        local my = (y0 + y1) * 0.5
        -- Back slopes first, then front slopes according to camera direction
        if cosY >= 0 then
            drawTri3D(x0, y0, z0, x1, y0, z0, mx, my, z1, r * 0.68, g * 0.68, b * 0.72, a)
        else
            drawTri3D(x0, y1, z0, x1, y1, z0, mx, my, z1, r * 0.68, g * 0.68, b * 0.72, a)
        end
        if sinY >= 0 then
            drawTri3D(x0, y0, z0, x0, y1, z0, mx, my, z1, r * 0.74, g * 0.74, b * 0.78, a)
            drawTri3D(x1, y0, z0, x1, y1, z0, mx, my, z1, r * 0.88, g * 0.88, b * 0.90, a)
        else
            drawTri3D(x1, y0, z0, x1, y1, z0, mx, my, z1, r * 0.74, g * 0.74, b * 0.78, a)
            drawTri3D(x0, y0, z0, x0, y1, z0, mx, my, z1, r * 0.92, g * 0.92, b * 0.94, a)
        end
        if cosY >= 0 then
            drawTri3D(x0, y1, z0, x1, y1, z0, mx, my, z1, r, g, b, a)
        else
            drawTri3D(x0, y0, z0, x1, y0, z0, mx, my, z1, r, g, b, a)
        end
    end

    -- Draw subtle 3D floor grid frame for each visible layer
    local maxRenderLayer = (cutawayMode3D == 2) and math.min(height, currentLayer) or height
    for z = 1, maxRenderLayer do
        local wz = (z - 1) * layerGap3D
        local isCur = (z == currentLayer)
        local bx0, by0 = 0.5, 0.5
        local bx1, by1 = width + 0.5, length + 0.5
        local s1x, s1y = project3D(bx0, by0, wz)
        local s2x, s2y = project3D(bx1, by0, wz)
        local s3x, s3y = project3D(bx1, by1, wz)
        local s4x, s4y = project3D(bx0, by1, wz)
        if isCur then
            love.graphics.setColor(0.35, 0.75, 1.0, 0.48)
        else
            love.graphics.setColor(0.22, 0.25, 0.32, 0.22)
        end
        love.graphics.polygon("line", s1x, s1y, s2x, s2y, s3x, s3y, s4x, s4y)
    end

    -- Exact back-to-front Painter's iteration order in X and Y based on camera yaw
    local xStart, xEnd, xStep = 1, width, 1
    if sinY < 0 then
        xStart, xEnd, xStep = width, 1, -1
    end
    local yStart, yEnd, yStep = 1, length, 1
    if cosY < 0 then
        yStart, yEnd, yStep = length, 1, -1
    end

    local sockets = ruleset and ruleset.sockets or {}
    local upperPairs = ruleset and ruleset.upperAnchorPairs or {}

    for z = 1, maxRenderLayer do
        local layerGrid = grid[z]
        local reachLayer = reachableGrid[z]
        local belowLayer = (z > 1) and grid[z - 1] or nil
        local wz = (z - 1) * layerGap3D

        -- Depth/layer brightness + focus opacity
        local layerShade = 0.78 + 0.22 * ((height > 1) and ((z - 1) / (height - 1)) or 1.0)
        local alpha = 1.0
        if cutawayMode3D == 3 and z ~= currentLayer then
            alpha = 0.16
        end

        if layerGrid then
            for y = yStart, yEnd, yStep do
                local row = layerGrid[y]
                local reachRow = reachLayer and reachLayer[y]
                local belowRow = belowLayer and belowLayer[y]
                if row then
                    for x = xStart, xEnd, xStep do
                        local tile = row[x]
                        if isValidTile(tile) and (showAir3D or not isAirSkyTile(tile)) then
                            local bc = tile.baseColor or SOLID_TILE_COLOR
                            local r = bc[1] * layerShade
                            local g = bc[2] * layerShade
                            local b = bc[3] * layerShade

                            if showTraversal and reachRow then
                                if reachRow[x] then
                                    r = r * 0.65 + 0.10
                                    g = g * 0.65 + 0.32
                                    b = b * 0.65 + 0.14
                                else
                                    r = r * 0.55 + 0.35
                                    g = g * 0.45 + 0.06
                                    b = b * 0.45 + 0.08
                                end
                            end

                            local catUpper = (tile.category or ""):upper()
                            local uSockUpper = (tile.sock.U or ""):upper()
                            local dSockUpper = (tile.sock.D or ""):upper()

                            local isPillar = catUpper:find("PILLAR", 1, true) ~= nil or uSockUpper:find("PILLAR", 1, true) ~= nil
                            local isRoof   = catUpper:find("ROOF", 1, true) ~= nil or uSockUpper:find("ROOF", 1, true) ~= nil
                            local isWaterDrop = dSockUpper:find("WATERFALL", 1, true) ~= nil or uSockUpper:find("WATERFALL", 1, true) ~= nil
                            local isBasin = uSockUpper:find("BASIN", 1, true) ~= nil

                            -- 1. Vertical support pillar from floor below if tile rests on PILLAR_DN
                            if dSockUpper:find("PILLAR", 1, true) and not isPillar then
                                drawBox3D(x - 0.18, y - 0.18, wz - 0.35, x + 0.18, y + 0.18, wz, 0.55 * layerShade, 0.60 * layerShade, 0.68 * layerShade, alpha)
                            end

                            -- 2. Main tile 3D geometry (supports full 6-directional N/E/S/W/U/D 3D pipes!)
                            local hasAnyPipeOpen = (tile.openCount and tile.openCount > 0) or (tile.open and (tile.open.U or tile.open.D))
                            if hasAnyPipeOpen and not isPillar then
                                -- 3D Pipe / Corridor with open directional arms in N, E, S, W, U, D
                                local slabH = 0.28
                                local armW  = 0.22
                                local chanCol = tile.socketTint or {r, g, b}
                                if chanCol[1] == 1 and chanCol[2] == 1 and chanCol[3] == 1 then
                                    chanCol = { math.min(1, r * 1.15 + 0.05), math.min(1, g * 1.15 + 0.05), math.min(1, b * 1.15 + 0.05) }
                                else
                                    chanCol = { chanCol[1] * layerShade, chanCol[2] * layerShade, chanCol[3] * layerShade }
                                end

                                -- Downward 3D pipe arm (D = -Z) drawn first (underneath center hub)
                                if tile.open and tile.open.D then
                                    local dzBot = wz - 0.5 * layerGap3D + slabH * 0.5
                                    drawBox3D(x - armW * 0.85, y - armW * 0.85, dzBot, x + armW * 0.85, y + armW * 0.85, wz, r * 0.92, g * 0.88, b * 0.82, alpha)
                                end

                                -- Back horizontal arms, center hub, front horizontal arms
                                local function drawArm(dir)
                                    local sName = tile.sock[dir]
                                    local aCol = (sName and sName ~= "OPEN" and sName ~= "WALL")
                                        and resolveSocketColor(sName, sockets[sName]) or chanCol
                                    local ar, ag, ab = aCol[1] * layerShade, aCol[2] * layerShade, aCol[3] * layerShade
                                    if dir == "N" and tile.open.N then
                                        drawBox3D(x - armW, y - 0.5, wz, x + armW, y - armW, wz + slabH, ar, ag, ab, alpha)
                                    elseif dir == "S" and tile.open.S then
                                        drawBox3D(x - armW, y + armW, wz, x + armW, y + 0.5, wz + slabH, ar, ag, ab, alpha)
                                    elseif dir == "W" and tile.open.W then
                                        drawBox3D(x - 0.5, y - armW, wz, x - armW, y + armW, wz + slabH, ar, ag, ab, alpha)
                                    elseif dir == "E" and tile.open.E then
                                        drawBox3D(x + armW, y - armW, wz, x + 0.5, y + armW, wz + slabH, ar, ag, ab, alpha)
                                    end
                                end

                                if cosY >= 0 then drawArm("N") else drawArm("S") end
                                if sinY >= 0 then drawArm("W") else drawArm("E") end

                                -- Center pipe joint hub
                                local hubExtra = (tile.open and (tile.open.U or tile.open.D)) and 0.04 or 0.02
                                drawBox3D(x - armW - hubExtra, y - armW - hubExtra, wz, x + armW + hubExtra, y + armW + hubExtra, wz + slabH + hubExtra, r, g, b, alpha)

                                if sinY >= 0 then drawArm("E") else drawArm("W") end
                                if cosY >= 0 then drawArm("S") else drawArm("N") end

                                -- Upward 3D pipe arm (U = +Z) rising from top of center hub to meet layer z+1's downward arm
                                if tile.open and tile.open.U and not tile.isLowerAnchor then
                                    local uzTop = wz + 0.5 * layerGap3D + slabH * 0.5
                                    drawBox3D(x - armW * 0.85, y - armW * 0.85, wz + slabH, x + armW * 0.85, y + armW * 0.85, uzTop, math.min(1, r * 1.12), math.min(1, g * 1.05), b * 0.95, alpha)
                                end

                                -- If this hub is also crowned with a roof/pagoda, draw 3D roof cap on top
                                if isRoof then
                                    drawPyramid3D(x - 0.42, y - 0.42, wz + slabH + 0.04, x + 0.42, y + 0.42, wz + 0.95, 0.90 * layerShade, 0.44 * layerShade, 0.32 * layerShade, alpha)
                                end
                            elseif isPillar then
                                -- Full-height 3D stone column shaft spanning the entire layer height
                                drawBox3D(x - 0.36, y - 0.36, wz, x + 0.36, y + 0.36, wz + 0.16, r * 0.85, g * 0.85, b * 0.88, alpha)
                                drawBox3D(x - 0.24, y - 0.24, wz + 0.16, x + 0.24, y + 0.24, wz + layerGap3D, r, g, b, alpha)
                            elseif isRoof then
                                -- Pedestal + 3D spire roof pyramid
                                drawBox3D(x - 0.42, y - 0.42, wz, x + 0.42, y + 0.42, wz + 0.22, 0.58 * layerShade, 0.62 * layerShade, 0.68 * layerShade, alpha)
                                drawPyramid3D(x - 0.46, y - 0.46, wz + 0.22, x + 0.46, y + 0.46, wz + 1.05, r, g, b, alpha)
                            else
                                -- Solid terrain / pixel / chamber block
                                drawBox3D(x - 0.47, y - 0.47, wz, x + 0.47, y + 0.47, wz + 0.22, r, g, b, alpha)
                            end

                            -- 3. Vertical 3D Waterfall cascade
                            if isWaterDrop then
                                drawBox3D(x - 0.18, y - 0.18, wz - layerGap3D + 0.22, x + 0.18, y + 0.18, wz + 0.26, 0.22, 0.78, 0.98, alpha * 0.78)
                            elseif isBasin then
                                drawBox3D(x - 0.38, y - 0.38, wz + 0.20, x + 0.38, y + 0.38, wz + 0.34, 0.12, 0.62, 0.92, alpha * 0.90)
                            end

                            -- 4. 3D Staircase / Vertical Connector Tower rising from layer z to z + 1
                            if tile.isLowerAnchor then
                                local steps = 4
                                local stepDz = (layerGap3D - 0.20) / steps
                                for s = 1, steps do
                                    local sz0 = wz + 0.20 + (s - 1) * stepDz
                                    local sz1 = sz0 + stepDz
                                    local frac = (s - 1) / steps
                                    local sy0 = (y - 0.30) + frac * 0.42
                                    local sy1 = sy0 + 0.18
                                    drawBox3D(x - 0.22, sy0, sz0, x + 0.22, sy1, sz1, 1.0, 0.42 + 0.12 * s, 0.18, alpha)
                                end
                            end

                            -- 5. Upper Stair Receiver Landing Ring
                            local pairsForUpper = upperPairs[tile.name]
                            if pairsForUpper and belowRow then
                                local belowTile = belowRow[x]
                                if isValidTile(belowTile) and pairsForUpper[belowTile.name] then
                                    drawBox3D(x - 0.34, y - 0.34, wz + 0.24, x + 0.34, y + 0.34, wz + 0.34, 0.22, 0.68, 1.0, alpha * 0.92)
                                end
                            end
                        end
                    end
                end
            end
        end
    end
end

-- =================================================
-- LÖVE CALLBACKS
-- =================================================

function love.load()
    love.window.setTitle("CGSME Inspector (Multi-Tileset Visualizer & Parts Kit)")
    love.window.setMode(1000, 1000, {resizable = true})
    love.graphics.setBackgroundColor(WINDOW_BG_COLOR)

    discoverTilesets()
    loadRuleset()
    loadMaze3D()
    loadMask()
    runFloodFill()
    centerCamera()
end

function love.update(dt)
    if statusTimer > 0 then
        statusTimer = math.max(0, statusTimer - dt)
    end
end

function love.draw()
    if showPalette then
        drawPaletteView()
    elseif view3D and width > 0 then
        draw3DView()
    elseif width > 0 then
        love.graphics.push()
        love.graphics.translate(camX, camY)
        love.graphics.scale(camZoom)

        local currentGrid = grid[currentLayer]
        local currentReachable = reachableGrid[currentLayer]
        local belowGrid = (currentLayer > 1) and grid[currentLayer - 1] or nil
        local upperPairs = ruleset and ruleset.upperAnchorPairs or {}

        local winW, winH = love.graphics.getWidth(), love.graphics.getHeight()
        local invScale = tileSize * camZoom
        local minX, maxX, minY, maxY = 1, width, 1, length
        if invScale > 0 then
            minX = math.max(1, math.floor((-camX) / invScale))
            maxX = math.min(width, math.ceil((winW - camX) / invScale) + 1)
            minY = math.max(1, math.floor((-camY) / invScale))
            maxY = math.min(length, math.ceil((winH - camY) / invScale) + 1)
        end

        if currentGrid then
            for y = minY, maxY do
                local row = currentGrid[y]
                local reachRow = currentReachable and currentReachable[y]
                local belowRow = belowGrid and belowGrid[y]
                local maskRow = showMask and maskGrid[y] or nil

                for x = minX, maxX do
                    local tile = row and row[x] or nil
                    local drawX = (x - 1) * tileSize
                    local drawY = (y - 1) * tileSize

                    if tile == nil then
                        love.graphics.setColor(VOID_COLOR)
                        love.graphics.rectangle("fill", drawX, drawY, tileSize, tileSize)
                    elseif tile.uncollapsed then
                        love.graphics.setColor(UNCOLLAPSED_COLOR)
                        love.graphics.rectangle("fill", drawX, drawY, tileSize, tileSize)
                    else
                        drawTileVisual(tile, drawX, drawY, tileSize)

                        if tile.isLowerAnchor then
                            love.graphics.setColor(1, 0, 0, 0.8)
                            love.graphics.circle("fill", drawX + tileSize / 2, drawY + tileSize / 2, tileSize / 4)
                        end
                        local pairsForUpper = upperPairs[tile.name]
                        if pairsForUpper and belowRow then
                            local belowTile = belowRow[x]
                            if isValidTile(belowTile) and pairsForUpper[belowTile.name] then
                                love.graphics.setColor(0, 0.5, 1, 0.8)
                                love.graphics.circle("fill", drawX + tileSize / 2, drawY + tileSize / 2, tileSize / 4)
                            end
                        end

                        if showTraversal then
                            local isReachable = reachRow and reachRow[x]
                            if isReachable then
                                love.graphics.setColor(REACHABLE_COLOR)
                            else
                                love.graphics.setColor(UNREACHABLE_COLOR)
                            end
                            love.graphics.rectangle("fill", drawX + 2, drawY + 2, tileSize - 4, tileSize - 4)
                        end
                    end

                    if maskRow and maskRow[x] then
                        love.graphics.setColor(1, 0, 0, 0.5)
                        love.graphics.rectangle("fill", drawX + 4, drawY + 4, tileSize - 8, tileSize - 8)
                    end
                end
            end
        end

        love.graphics.pop()
    end

    -- UI HUD (kept clean at lines y=10..170)
    love.graphics.setColor(1, 1, 1)
    love.graphics.print("Layer: " .. currentLayer .. " / " .. height, 10, 10)
    love.graphics.print(string.format("Maze Size: %d x %d [%s]", width, length, activeMazePath), 10, 30)

    local currentGrid = grid[currentLayer]
    local currentReachable = reachableGrid[currentLayer]
    local totalCells = width * length
    local filledCells = 0
    if currentGrid then
        for y = 1, length do
            local row = currentGrid[y]
            if row then
                for x = 1, width do
                    if isValidTile(row[x]) then
                        filledCells = filledCells + 1
                    end
                end
            end
        end
    end
    local fillPct = totalCells > 0 and (filledCells / totalCells * 100) or 0
    love.graphics.print(string.format("Fill: %d/%d (%.2f%%)", filledCells, totalCells, fillPct), 10, 50)

    if showMask then
        love.graphics.setColor(1, 0.2, 0.2)
        love.graphics.print("MASK OVERLAY: ON (Red = C Code marked this VALID)", 10, 70)
    else
        love.graphics.setColor(0.5, 0.5, 0.5)
        love.graphics.print("MASK OVERLAY: OFF (Press 'M')", 10, 70)
    end

    if showTraversal then
        love.graphics.setColor(0.2, 1, 0.4)
        love.graphics.print("TRAVERSAL: ON (Green=Reachable, DarkRed=Unreachable) (Press 'T')", 10, 90)
        local totalValid, totalReachable = 0, 0
        if currentGrid and currentReachable then
            for y = 1, length do
                local row = currentGrid[y]
                local reachRow = currentReachable[y]
                if row then
                    for x = 1, width do
                        if isValidTile(row[x]) then
                            totalValid = totalValid + 1
                            if reachRow and reachRow[x] then
                                totalReachable = totalReachable + 1
                            end
                        end
                    end
                end
            end
        end
        local pct = totalValid > 0 and (totalReachable / totalValid * 100) or 0
        local pct3D = totalValid3D > 0 and (totalReachable3D / totalValid3D * 100) or 0
        love.graphics.setColor(pct3D < 100 and 1 or 0.3, pct3D < 100 and 0.3 or 1, 0.3)
        love.graphics.print(string.format(
            "Reachable (Layer %d): %d/%d (%.2f%%) | 3D Volume Total: %d/%d (%.2f%%)",
            currentLayer, totalReachable, totalValid, pct,
            totalReachable3D, totalValid3D, pct3D), 10, 110)
    else
        love.graphics.setColor(0.5, 0.5, 0.5)
        love.graphics.print("TRAVERSAL: OFF (Press 'T')", 10, 90)
    end

    if ruleset then
        love.graphics.setColor(0.75, 0.85, 1.0)
        love.graphics.print(string.format(
            "Tileset [%d/%d]: %s (%d tiles, %d-bit | png:%d mock:%d proc:%d) [%s]",
            activeTilesetIdx, #discoveredTilesets,
            ruleset.name, ruleset.numTiles, ruleset.containerBits,
            ruleset.sourceCounts.png, ruleset.sourceCounts.mock, ruleset.sourceCounts.proc,
            ruleset.sourcePath), 10, 130)
    end

    local cutModes = { "ALL LAYERS", "CUTAWAY <= LAYER", "HIGHLIGHT LAYER" }
    love.graphics.setColor(0.45, 0.95, 0.85)
    love.graphics.print(string.format(
        "View: %s (Press 'V'/'3') | 3D Mode: %s ('F') | Gap: %.2f ('E'/'Q') | Air: %s ('A') | LMB Drag: Orbit 3D | RMB Drag: Pan | Wheel: Zoom",
        view3D and "3D ORBIT" or "2D SLICE",
        cutModes[cutawayMode3D] or "ALL",
        layerGap3D,
        showAir3D and "SHOW" or "HIDE"), 10, 150)

    love.graphics.setColor(1, 1, 1)
    love.graphics.print("Controls: (R) Reload, ([/]) Switch Tileset, (V/3) 3D/2D, (F) 3D Cutaway, (E/Q) 3D Gap, (A) Air, (P) Atlas, (G) Export PNG, (C) Center, (+/-) Layer", 10, 168)

    if uncollapsedCount > 0 then
        love.graphics.setColor(1, 0.4, 0.9)
        love.graphics.print(string.format("WARNING: %d uncollapsed/unknown tile IDs in %s for active tileset.", uncollapsedCount, activeMazePath), 10, 186)
    elseif statusTimer > 0 and statusMessage ~= "" then
        love.graphics.setColor(1, 0.9, 0.35)
        love.graphics.print(statusMessage, 10, 186)
    end

    -- Hover inspector tooltip on bottom-left when in 2D grid mode
    if not showPalette and not view3D and width > 0 and love.mouse and love.mouse.getPosition and camZoom > 0 then
        local mx, my = love.mouse.getPosition()
        local gx = math.floor((mx - camX) / (tileSize * camZoom)) + 1
        local gy = math.floor((my - camY) / (tileSize * camZoom)) + 1
        local layerGrid = grid[currentLayer]
        if gx >= 1 and gx <= width and gy >= 1 and gy <= length and layerGrid and layerGrid[gy] then
            local cell = layerGrid[gy][gx]
            local desc
            if cell == nil then
                desc = string.format("(%d,%d) VOID (0)", gx, gy)
            elseif cell.uncollapsed then
                desc = string.format("(%d,%d) UNCOLLAPSED / UNKNOWN", gx, gy)
            else
                desc = string.format("(%d,%d,z=%d) #%d %s [%s | %s] N:%s E:%s S:%s W:%s U:%s D:%s",
                    gx, gy, currentLayer, cell.id, cell.name,
                    cell.category ~= "" and cell.category or "-",
                    cell.sourceKind, cell.sock.N, cell.sock.E, cell.sock.S, cell.sock.W,
                    cell.sock.U or "-", cell.sock.D or "-")
            end
            love.graphics.setColor(0.05, 0.05, 0.08, 0.85)
            love.graphics.rectangle("fill", 8, love.graphics.getHeight() - 30, love.graphics.getWidth() - 16, 22)
            love.graphics.setColor(0.95, 0.95, 0.98)
            love.graphics.print(desc, 14, love.graphics.getHeight() - 26)
        end
    end
end

function centerCamera()
    if width == 0 then return end
    local scaleX = (love.graphics.getWidth() - 50) / (width * tileSize)
    local scaleY = (love.graphics.getHeight() - 50) / (length * tileSize)
    camZoom = math.min(scaleX, scaleY)
    local mazePixelW = width * tileSize * camZoom
    local mazePixelH = length * tileSize * camZoom
    camX = (love.graphics.getWidth() - mazePixelW) / 2
    camY = (love.graphics.getHeight() - mazePixelH) / 2

    yaw3D = 0.68
    pitch3D = 0.62
    zoom3D = 1.0
    pan3DX, pan3DY = 0, 0
end

function love.wheelmoved(x, y)
    if showPalette then
        if love.keyboard and love.keyboard.isDown and (love.keyboard.isDown("lctrl") or love.keyboard.isDown("rctrl")) then
            paletteZoom = math.max(0.5, math.min(2.5, paletteZoom * (y > 0 and 1.1 or 0.9)))
        else
            paletteScrollY = math.max(0, paletteScrollY - y * 48)
        end
        return
    end
    if view3D then
        if y > 0 then
            zoom3D = math.min(6.0, zoom3D * 1.12)
        elseif y < 0 then
            zoom3D = math.max(0.2, zoom3D / 1.12)
        end
        return
    end
    local mouseX, mouseY = love.mouse.getPosition()
    local oldZoom = camZoom
    if y > 0 then camZoom = camZoom * 1.1 elseif y < 0 then camZoom = camZoom / 1.1 end
    camX = mouseX - (mouseX - camX) * (camZoom / oldZoom)
    camY = mouseY - (mouseY - camY) * (camZoom / oldZoom)
end

function love.mousemoved(x, y, dx, dy)
    if showPalette then return end
    if view3D then
        if love.mouse.isDown(1) then
            yaw3D = yaw3D - dx * 0.008
            pitch3D = math.max(0.08, math.min(1.52, pitch3D + dy * 0.007))
        elseif love.mouse.isDown(2) or love.mouse.isDown(3) then
            pan3DX = pan3DX + dx
            pan3DY = pan3DY + dy
        end
        return
    end
    if love.mouse.isDown(1) or love.mouse.isDown(2) or love.mouse.isDown(3) then
        camX = camX + dx
        camY = camY + dy
    end
end

local function switchTilesetByDelta(delta)
    local prevPath = discoveredTilesets[activeTilesetIdx]
    discoverTilesets()
    if #discoveredTilesets == 0 then return end
    if prevPath then
        for i, p in ipairs(discoveredTilesets) do
            if p == prevPath then activeTilesetIdx = i; break end
        end
    end
    activeTilesetIdx = ((activeTilesetIdx - 1 + delta) % #discoveredTilesets) + 1
    clearImageCache()
    loadRuleset(discoveredTilesets[activeTilesetIdx])
    loadMaze3D()
    loadMask()
    runFloodFill()
    centerCamera()
    paletteScrollY = 0
    setStatus(string.format("Switched to tileset [%d/%d]: %s", activeTilesetIdx, #discoveredTilesets, ruleset.name))
end

function love.keypressed(key)
    if key == "r" then
        clearImageCache()
        discoverTilesets()
        loadRuleset()
        loadMaze3D()
        loadMask()
        runFloodFill()
    elseif key == "]" then
        switchTilesetByDelta(1)
    elseif key == "[" then
        switchTilesetByDelta(-1)
    elseif key == "p" then
        showPalette = not showPalette
        paletteScrollY = 0
    elseif key == "g" then
        local shiftHeld = love.keyboard and love.keyboard.isDown and (love.keyboard.isDown("lshift") or love.keyboard.isDown("rshift"))
        exportTilesetImages(shiftHeld)
    elseif key == "escape" then
        love.event.quit()
    elseif key == "c" then
        centerCamera()
    elseif key == "v" or key == "3" then
        view3D = not view3D
        setStatus(view3D and "Switched to 3D Orbit View (LMB drag to rotate, RMB drag to pan, E/Q layer gap, F cutaway)" or "Switched to 2D Slice View")
    elseif key == "f" then
        cutawayMode3D = (cutawayMode3D % 3) + 1
        local names = { "ALL LAYERS", "CUTAWAY <= CURRENT LAYER", "HIGHLIGHT CURRENT LAYER" }
        setStatus("3D Layer Focus: " .. names[cutawayMode3D])
    elseif key == "e" or key == "up" then
        layerGap3D = math.min(6.0, layerGap3D + 0.25)
    elseif key == "q" or key == "down" then
        layerGap3D = math.max(0.6, layerGap3D - 0.25)
    elseif key == "a" then
        showAir3D = not showAir3D
        setStatus("3D Air/Sky Blocks: " .. (showAir3D and "VISIBLE" or "HIDDEN"))
    elseif key == "m" then
        showMask = not showMask
    elseif key == "t" then
        showTraversal = not showTraversal
    elseif key == "=" or key == "kp+" then
        if currentLayer < height then currentLayer = currentLayer + 1 end
    elseif key == "-" or key == "kp-" then
        if currentLayer > 1 then currentLayer = currentLayer - 1 end
    end
end
