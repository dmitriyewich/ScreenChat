script_name("ScreenChat1")
script_author("Orc")
script_version("1.3")

local memory = require("memory")
local ffi = require("ffi")

ffi.cdef([[
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef int HRESULT;
typedef int D3DFORMAT;
typedef int D3DRESOURCETYPE;
typedef int D3DPOOL;
typedef int D3DMULTISAMPLE_TYPE;
typedef long LONG;

typedef struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT;

typedef struct D3DSURFACE_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    UINT Width;
    UINT Height;
} D3DSURFACE_DESC;

HRESULT D3DXSaveSurfaceToFileA(
    const char* pDestFile,
    int DestFormat,
    void* pSrcSurface,
    const void* pSrcPalette,
    const RECT* pSrcRect
);
]])

local hotkey = 0x77

local capture_directory_name = "screens"
local capture_file_prefix = "chat1"

local cast = ffi.cast
local new = ffi.new
local NULL = ffi.NULL

local D3DX_LIBS = {
	"d3dx9_43",
	"d3dx9_42",
	"d3dx9_41",
	"d3dx9_40",
}

local D3DXIFF_PNG = 3
local D3DPOOL_SYSTEMMEM = 2
local DT_CALCRECT = 0x00000400

local DEFAULT_CHAT_X = 45
local DEFAULT_CHAT_Y = 10

local currentVersion, sampModule = nil, nil

local entryPoint = {
	[0x31DF13] = { "R1", true },
	[0x3195DD] = { "R2", true },
	[0xCC490] = { "R3-1", true },
	[0xCC4D0] = { "R3-1", true },
	[0xCBCD0] = { "R4", true },
	[0xCBCB0] = { "R4-2", true },
	[0xCBC90] = { "R5-2", true },
	[0xFDB60] = { "DL-R1", true },
}

local main_offsets = {
	["pChat"] = {
		["R1"] = 0x21A0E4,
		["R2"] = 0x21A0EC,
		["R3-1"] = 0x26E8C8,
		["R4"] = 0x26E9F8,
		["R4-2"] = 0x26E9F8,
		["R5-2"] = 0x26EB80,
		["DL-R1"] = 0x2ACA10,
	},
	["chat"] = {
		SCROLL = 0x11E,
		TIMESTAMP_ENABLED = 0x0C,
		FONTS = 0x63A2,
		TEXTURE = 0x63BA,
		SURFACE = 0x63BE,
		STRING_HEIGHT = 0x63E2,
		TIMESTAMP_WIDTH = 0x63E6,
		ENTRIES = 0x132,
		ENTRY_SIZE = 0xFC,
		ENTRY_COUNT = 0x64,
	},
	["entry"] = {
		PREFIX = 0x04,
		PREFIX_LEN = 0x1C,
		TEXT = 0x20,
		TEXT_LEN = 0x90,
	},
	["scroll"] = {
		CURRENT_POS = 142,
		PAGE_SIZE = 146,
	},
	["font"] = {
		CHAT_FONT = 0,
	},
	["chat_position"] = {
		["R1"] = { x = 0x63DB1, y = 0x63DA0 },
		["R3-1"] = { x = 0x67201, y = 0x671F0 },
		["R5-2"] = { x = 0x67981, y = 0x67217 }, -- -- x 0x67981,  x 0x67ba6, y 0x67217, y 0x67970, y 0x683f1
		["DL-R1"] = { x = 0x673F1, y = 0x673E0 },
	},
}

local function joinPath(base_path, child_name)
	return ("%s\\%s"):format(base_path, child_name)
end

local function getSaveDirectory()
	return joinPath(getWorkingDirectory(), capture_directory_name)
end

local function makeCapturePath()
	local directory = getSaveDirectory()
	local stamp = os.date("%Y%m%d_%H%M%S")
	local file_stem = ("%s_%s"):format(capture_file_prefix, stamp)
	local path = joinPath(directory, file_stem .. ".png")

	if not doesFileExist(path) then
		return path
	end

	local suffix = 1
	repeat
		path = joinPath(directory, ("%s_%02d.png"):format(file_stem, suffix))
		suffix = suffix + 1
	until not doesFileExist(path)

	return path
end

local function bindMethod(vtbl, index, signature)
	return cast(signature, vtbl[index])
end

local function hrToHex(hr)
	local n = tonumber(hr) or 0

	if n < 0 then
		n = n + 0x100000000
	end

	return ("0x%08X"):format(n)
end

local function isNull(ptr)
	return ptr == nil or ptr == NULL
end

local function tryLoadD3dx()
	for i = 1, #D3DX_LIBS do
		local ok, lib = pcall(ffi.load, D3DX_LIBS[i])
		if ok and lib then
			return lib, D3DX_LIBS[i]
		end
	end

	return nil, nil
end

local function releaseCom(ptr)
	if isNull(ptr) then
		return
	end

	local vtbl = cast("void***", ptr)[0]
	local release = cast("ULONG(__stdcall*)(void*)", vtbl[2])
	release(ptr)
end

local function asBytePtr(base)
	if base == nil or base == 0 then
		return nil
	end

	return cast("uint8_t*", base)
end

local function readPtr(base, offset)
	local ptr = asBytePtr(base)
	if not ptr then
		return nil
	end

	return cast("void**", ptr + offset)[0]
end

local function readInt32(base, offset)
	local ptr = asBytePtr(base)
	if not ptr then
		return 0
	end

	return tonumber(cast("int32_t*", ptr + offset)[0]) or 0
end

local function readBool(base, offset)
	local ptr = asBytePtr(base)
	if not ptr then
		return false
	end

	return (ptr + offset)[0] ~= 0
end

local function readCString(base, offset, max_len)
	local ptr = asBytePtr(base)
	if not ptr then
		return ""
	end

	local raw = ffi.string(cast("char*", ptr + offset), max_len)
	local zero_at = raw:find("\0", 1, true)

	if zero_at then
		return raw:sub(1, zero_at - 1)
	end

	return raw
end

local function stripColorCodes(text)
	return (text:gsub("{%x%x%x%x%x%x}", ""))
end

local function clamp(value, min_value, max_value)
	if value < min_value then
		return min_value
	end

	if value > max_value then
		return max_value
	end

	return value
end

local function copyTextureLevel0ToSystemSurface(tex)
	if isNull(tex) then
		return nil, nil, nil, "texture is null"
	end

	local tex_vtbl = cast("void***", tex)[0]

	local getLevelDesc = bindMethod(tex_vtbl, 17, "HRESULT(__stdcall*)(void* self, UINT Level, D3DSURFACE_DESC* pDesc)")
	local getSurfaceLevel =
		bindMethod(tex_vtbl, 18, "HRESULT(__stdcall*)(void* self, UINT Level, void** ppSurfaceLevel)")

	local desc = new("D3DSURFACE_DESC[1]")
	local hr = getLevelDesc(tex, 0, desc)
	if hr ~= 0 then
		return nil, nil, nil, "GetLevelDesc failed: " .. hrToHex(hr)
	end

	local src_surface = new("void*[1]")
	hr = getSurfaceLevel(tex, 0, src_surface)
	if hr ~= 0 or isNull(src_surface[0]) then
		return nil, nil, nil, "GetSurfaceLevel failed: " .. hrToHex(hr)
	end

	local device = cast("void*", getD3DDevicePtr())
	if isNull(device) then
		releaseCom(src_surface[0])
		return nil, nil, nil, "getD3DDevicePtr returned null"
	end

	local dev_vtbl = cast("void***", device)[0]
	local getRenderTargetData =
		bindMethod(dev_vtbl, 32, "HRESULT(__stdcall*)(void* self, void* pRenderTarget, void* pDestSurface)")

	local createOffscreenPlainSurface = bindMethod(
		dev_vtbl,
		36,
		"HRESULT(__stdcall*)(void* self, UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, void** ppSurface, void* pSharedHandle)"
	)

	local sys_surface = new("void*[1]")
	hr = createOffscreenPlainSurface(
		device,
		desc[0].Width,
		desc[0].Height,
		desc[0].Format,
		D3DPOOL_SYSTEMMEM,
		sys_surface,
		nil
	)

	if hr ~= 0 or isNull(sys_surface[0]) then
		releaseCom(src_surface[0])
		return nil, nil, nil, "CreateOffscreenPlainSurface failed: " .. hrToHex(hr)
	end

	hr = getRenderTargetData(device, src_surface[0], sys_surface[0])
	releaseCom(src_surface[0])

	if hr ~= 0 then
		releaseCom(sys_surface[0])
		return nil, nil, nil, "GetRenderTargetData failed: " .. hrToHex(hr)
	end

	return sys_surface[0], tonumber(desc[0].Width), tonumber(desc[0].Height)
end

local function isSampLoadedLua()
	if sampModule <= 0 then
		return false
	end

	if not currentVersion then
		local ep = memory.getuint32(sampModule + memory.getint32(sampModule + 0x3C, false) + 0x28, false)
		local info = entryPoint[ep]
		assert(info, ("Unknown version of SA-MP (Entry point: 0x%X)"):format(ep))

		currentVersion = info[1]
		if not info[2] then
			print("Samp version " .. currentVersion .. " is not supported")
			thisScript():unload()
		end
	end

	return true
end

local function getChatObject()
	local chat = memory.getint32(sampModule + main_offsets.pChat[currentVersion], true)
	if not chat or chat == 0 then
		return nil
	end

	return cast("uint8_t*", chat)
end

local function getChatTexture(chat_ptr)
	return readPtr(chat_ptr, main_offsets.chat.TEXTURE)
end

local function getChatSurface(chat_ptr)
	return readPtr(chat_ptr, main_offsets.chat.SURFACE)
end

local function getChatOrigin()
	local version_offsets = main_offsets.chat_position[currentVersion]
	if version_offsets then
		local x = memory.getint32(sampModule + version_offsets.x, true)
		local y = memory.getint32(sampModule + version_offsets.y, true)
		if x and y and x >= 0 and y >= 0 then
			return x, y
		end
	end

	return DEFAULT_CHAT_X, DEFAULT_CHAT_Y
end

local function getScrollWindow(chat_ptr)
	local current_pos = 0
	local page_size = readInt32(chat_ptr, 0)
	local scroll = readPtr(chat_ptr, main_offsets.chat.SCROLL)

	if not isNull(scroll) then
		current_pos = readInt32(scroll, main_offsets.scroll.CURRENT_POS)
		local scroll_page_size = readInt32(scroll, main_offsets.scroll.PAGE_SIZE)
		if scroll_page_size > 0 then
			page_size = scroll_page_size
		end
	end

	if page_size <= 0 then
		page_size = main_offsets.chat.ENTRY_COUNT
	end

	current_pos = clamp(current_pos, 0, main_offsets.chat.ENTRY_COUNT)
	local to_index = clamp(current_pos + page_size, 0, main_offsets.chat.ENTRY_COUNT)

	return current_pos, to_index
end

local function getLineStep(chat_ptr)
	local string_height = readInt32(chat_ptr, main_offsets.chat.STRING_HEIGHT)
	if string_height <= 0 then
		string_height = 12
	end

	return string_height + 1, string_height
end

local function getEntryStrings(chat_ptr, index)
	local entry = chat_ptr + main_offsets.chat.ENTRIES + index * main_offsets.chat.ENTRY_SIZE
	local prefix = stripColorCodes(readCString(entry, main_offsets.entry.PREFIX, main_offsets.entry.PREFIX_LEN))
	local text = stripColorCodes(readCString(entry, main_offsets.entry.TEXT, main_offsets.entry.TEXT_LEN))

	return prefix, text
end

local function isLineEmpty(prefix, text)
	if prefix ~= "" then
		return false
	end

	if text == "" then
		return true
	end

	return text == " "
end

local function getChatFont(chat_ptr)
	local font_set = readPtr(chat_ptr, main_offsets.chat.FONTS)
	if isNull(font_set) then
		return nil
	end

	return readPtr(font_set, main_offsets.font.CHAT_FONT)
end

local function measureTextWidth(font, text)
	if isNull(font) or text == "" then
		return 0
	end

	local font_vtbl = cast("void***", font)[0]
	local drawTextA = bindMethod(
		font_vtbl,
		14,
		"int(__stdcall*)(void* self, void* pSprite, const char* pString, int Count, RECT* pRect, DWORD Format, DWORD Color)"
	)

	local rc = new("RECT[1]")
	rc[0].left = 0
	rc[0].top = 0
	rc[0].right = 0
	rc[0].bottom = 0

	drawTextA(font, nil, text, -1, rc, DT_CALCRECT, 0xFFFFFFFF)
	return math.max(0, tonumber(rc[0].right - rc[0].left))
end

local function buildChatRect(chat_ptr)
	local from_index, to_index = getScrollWindow(chat_ptr)
	if to_index <= from_index then
		return nil, "visible range is empty"
	end

	local first_nonempty, last_nonempty = nil, nil

	for i = from_index, to_index - 1 do
		local prefix, text = getEntryStrings(chat_ptr, i)
		if not isLineEmpty(prefix, text) then
			if not first_nonempty then
				first_nonempty = i
			end
			last_nonempty = i
		end
	end

	if not first_nonempty then
		return nil, "visible area not found"
	end

	local line_step, string_height = getLineStep(chat_ptr)
	local origin_x, origin_y = getChatOrigin()
	local font = getChatFont(chat_ptr)
	local max_width = 0

	for i = first_nonempty, last_nonempty do
		local prefix, text = getEntryStrings(chat_ptr, i)
		local line_width = 0

		if prefix ~= "" then
			line_width = line_width + measureTextWidth(font, prefix) + 5
		end

		if text ~= "" and text ~= " " then
			line_width = line_width + measureTextWidth(font, text)
		end

		if line_width > max_width then
			max_width = line_width
		end
	end

	if readBool(chat_ptr, main_offsets.chat.TIMESTAMP_ENABLED) then
		max_width = max_width + math.max(0, readInt32(chat_ptr, main_offsets.chat.TIMESTAMP_WIDTH)) + 5
	end

	local horizontal_padding = clamp(math.floor(string_height * 0.35), 2, 8)
	local vertical_padding = clamp(math.floor(string_height * 0.20), 1, 4)
	local outer_padding = 5

	local raw_top = origin_y + (first_nonempty - from_index) * line_step
	local raw_bottom = raw_top + (last_nonempty - first_nonempty + 1) * line_step

	local rc = new("RECT[1]")
	rc[0].left = math.max(0, origin_x - 1 - outer_padding)
	rc[0].top = math.max(0, raw_top - vertical_padding - outer_padding)
	rc[0].right = math.max(rc[0].left + 1, origin_x + max_width + horizontal_padding + outer_padding)
	rc[0].bottom = math.max(rc[0].top + 1, raw_bottom + vertical_padding + outer_padding)

	return rc
end

local function saveRectFromSurface(d3dx, surface, rc, path)
	local hr = d3dx.D3DXSaveSurfaceToFileA(path, D3DXIFF_PNG, surface, nil, rc)
	if hr ~= 0 then
		return false, "D3DXSaveSurfaceToFileA failed: " .. hrToHex(hr)
	end

	return true
end

local function saveVisibleChatArea(d3dx)
	if not d3dx then
		return false, nil, "d3dx9_xx.dll not found"
	end

	local chat_ptr = getChatObject()
	if not chat_ptr then
		return false, nil, "chat not found"
	end

	local rc, rect_err = buildChatRect(chat_ptr)
	if not rc then
		return false, nil, rect_err
	end

	local path = makeCapturePath()
	local surface = getChatSurface(chat_ptr)

	if not isNull(surface) then
		local ok, save_err = saveRectFromSurface(d3dx, surface, rc, path)
		if ok then
			return true, path
		end
	end

	local texture = getChatTexture(chat_ptr)
	if isNull(texture) then
		return false, nil, "chat surface and texture are null"
	end

	local sys_surface, _, _, copy_err = copyTextureLevel0ToSystemSurface(texture)
	if not sys_surface then
		return false, nil, copy_err
	end

	local ok, save_err = saveRectFromSurface(d3dx, sys_surface, rc, path)
	releaseCom(sys_surface)

	if not ok then
		return false, nil, save_err
	end

	return true, path
end

local function ensureDir(path)
	if doesDirectoryExist(path) then
		return true
	end

	return createDirectory(path)
end

function main()
	repeat wait(0) until memory.read(0xC8D4C0, 4, false) == 9
	sampModule = getModuleHandle("samp.dll")
	repeat wait(0) until isSampLoadedLua()

	local save_directory = getSaveDirectory()
	if not ensureDir(save_directory) then
		print("Failed to create directory: " .. save_directory)
		return
	end

	local d3dx = tryLoadD3dx()
	if not d3dx then
		print("d3dx9_xx.dll not found")
	end

	while true do
		wait(0)

		if isKeyJustPressed(hotkey) then
			local ok, _, err = saveVisibleChatArea(d3dx)
			if not ok then
				print("Error: " .. tostring(err))
			end
		end
	end
end
