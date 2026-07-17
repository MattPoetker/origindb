// =============================================================================
// OriginDB C# WASM Module SDK
//
// Single-file SDK for writing OriginDB server-side modules in C#, targeting
// .NET 8 / wasi-wasm (Mono, `wasi-experimental` workload). Compile this file
// into your module project:
//
//   <Compile Include="path/to/sdk/csharp/OriginDB.cs" />
//
// It implements the OriginDB WASM ABI v1 (docs/WASM_ABI.md):
//
//   * Host imports from module "env" (host_table_read, host_table_write, ...)
//     declared with [WasmImportLinkage] and raw pointer signatures. All UTF-8
//     marshalling is done manually — [MarshalAs(LPUTF8Str)] is unreliable
//     under wasi trimming.
//   * The four guest exports — origindb_alloc, origindb_free,
//     origindb_describe, origindb_invoke — implemented once, here, with
//     [UnmanagedCallersOnly]. Your module must NOT define them again.
//   * A ReducerRegistry (see `Reducers`) that origindb_invoke dispatches
//     through. Unregistered names return -404 per the ABI.
//
// Authoring pattern:
//
//   public static class Program
//   {
//       public static void Main() { }   // WASI command entry; may stay empty.
//
//       [System.Runtime.CompilerServices.ModuleInitializer]
//       internal static void Init()
//       {
//           Reducers.SetModuleInfo("my_module", "1.0.0");
//           Reducers.Register("CreateUser", args => { ... }, "name", "email");
//           Reducers.RegisterFilter("OnlyInserts", ev => ...);
//       }
//   }
//
// The [ModuleInitializer] is guaranteed to run before any other code in the
// assembly executes — including the [UnmanagedCallersOnly] exports below — so
// the registry is always populated before the host's first origindb_invoke /
// origindb_describe call, regardless of whether the host runs `_initialize`,
// `_start`, or neither.
// =============================================================================

#nullable enable

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

#if !NET9_0_OR_GREATER
namespace System.Runtime.InteropServices
{
    // Polyfill: WasmImportLinkageAttribute exists in the .NET 8 wasm/wasi
    // targeting packs but not in the neutral net8.0 ref pack. The runtime
    // matches the attribute by its full metadata name, so this source-level
    // definition behaves identically when publishing for wasi-wasm. (If the
    // real type is also visible, the compiler prefers this one with a CS0436
    // warning — harmless.)
    [AttributeUsage(AttributeTargets.Method, Inherited = false)]
    internal sealed class WasmImportLinkageAttribute : Attribute { }
}
#endif

namespace OriginDB
{
    /// <summary>Log levels accepted by host_log.</summary>
    public enum LogLevel
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
    }

    /// <summary>
    /// Throw from a reducer to fail the call with a specific negative ABI
    /// status code (default -1). Staged writes/events are discarded by the
    /// host on any negative status. Do not use -404 (reserved for
    /// "no handler registered").
    /// </summary>
    public class ReducerException : Exception
    {
        public int StatusCode { get; }

        public ReducerException(string message, int statusCode = -1) : base(message)
        {
            // Negative per ABI; -404 is reserved for the dispatcher itself.
            StatusCode = (statusCode >= 0 || statusCode == -404) ? -1 : statusCode;
        }
    }

    /// <summary>Thrown when a host table/event call returns a negative code.</summary>
    public class HostException : Exception
    {
        public int StatusCode { get; }

        public HostException(string operation, int statusCode)
            : base($"{operation} failed with host status {statusCode}")
        {
            StatusCode = statusCode < 0 ? statusCode : -1;
        }
    }

    /// <summary>
    /// Wrap a string that is already JSON to have it passed through verbatim
    /// as a reducer result instead of being serialized as a JSON string.
    /// </summary>
    public readonly struct RawJson
    {
        public readonly string Json;
        public RawJson(string json) => Json = json;
        public override string ToString() => Json;
    }

    // =========================================================================
    // Host imports (module "env") — raw ABI signatures. See docs/WASM_ABI.md.
    // =========================================================================
    internal static unsafe class Env
    {
        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_table_read")]
        internal static extern int TableRead(byte* table, byte* key, int keyLen, int* outPtr, int* outLen);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_table_write")]
        internal static extern int TableWrite(byte* table, byte* key, int keyLen, byte* value, int valueLen);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_table_delete")]
        internal static extern int TableDelete(byte* table, byte* key, int keyLen);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_table_scan")]
        internal static extern int TableScan(byte* table, byte* prefix, int prefixLen, int limit, int* outPtr, int* outLen);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_emit_event")]
        internal static extern int EmitEvent(byte* topic, byte* key, int keyLen, byte* payload, int payloadLen);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_now_ms")]
        internal static extern long NowMs();

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_generate_id")]
        internal static extern long GenerateId();

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_log")]
        internal static extern void Log(int level, byte* msg);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_abort")]
        internal static extern void Abort(byte* msg);

        [WasmImportLinkage]
        [DllImport("env", EntryPoint = "host_set_result")]
        internal static extern void SetResult(byte* ptr, int len);
    }

    // =========================================================================
    // Manual UTF-8 marshalling helpers.
    // =========================================================================
    internal static unsafe class Mem
    {
        /// <summary>Allocate a NUL-terminated UTF-8 copy of <paramref name="s"/>.</summary>
        internal static byte* CString(string s)
        {
            int n = Encoding.UTF8.GetByteCount(s);
            byte* p = (byte*)NativeMemory.Alloc((nuint)(n + 1));
            if (n > 0)
            {
                fixed (char* c = s)
                {
                    Encoding.UTF8.GetBytes(c, s.Length, p, n);
                }
            }
            p[n] = 0;
            return p;
        }

        /// <summary>Allocate a non-terminated UTF-8 copy of <paramref name="s"/> (ptr+len style).</summary>
        internal static byte* Bytes(string s, out int len)
        {
            len = Encoding.UTF8.GetByteCount(s);
            byte* p = (byte*)NativeMemory.Alloc((nuint)(len == 0 ? 1 : len));
            if (len > 0)
            {
                fixed (char* c = s)
                {
                    Encoding.UTF8.GetBytes(c, s.Length, p, len);
                }
            }
            return p;
        }

        internal static void Free(byte* p)
        {
            if (p != null) NativeMemory.Free(p);
        }

        /// <summary>
        /// Decode and free a guest buffer that the host filled via our
        /// origindb_alloc (which uses NativeMemory.Alloc — same allocator).
        /// </summary>
        internal static string TakeGuestBuffer(int ptr, int len)
        {
            if (ptr == 0) return "";
            string s = len > 0 ? Encoding.UTF8.GetString((byte*)(nint)ptr, len) : "";
            NativeMemory.Free((void*)(nint)ptr);
            return s;
        }
    }

    // =========================================================================
    // High-level host API.
    // =========================================================================

    /// <summary>Utility host functions: time, ids, logging, abort, events, results.</summary>
    public static unsafe class Host
    {
        /// <summary>Wall clock in ms — fixed for the duration of the current call.</summary>
        public static long NowMs() => Env.NowMs();

        /// <summary>Unique id: (now_ms &lt;&lt; 20) | counter.</summary>
        public static long GenerateId() => Env.GenerateId();

        public static void Log(LogLevel level, string message)
        {
            byte* p = Mem.CString(message ?? "");
            try { Env.Log((int)level, p); }
            finally { Mem.Free(p); }
        }

        public static void LogTrace(string message) => Log(LogLevel.Trace, message);
        public static void LogDebug(string message) => Log(LogLevel.Debug, message);
        public static void LogInfo(string message) => Log(LogLevel.Info, message);
        public static void LogWarn(string message) => Log(LogLevel.Warn, message);
        public static void LogError(string message) => Log(LogLevel.Error, message);

        /// <summary>
        /// Records the message and traps. The current call fails and all
        /// staged writes/events are discarded. Does not return.
        /// </summary>
        public static void Abort(string message)
        {
            byte* p = Mem.CString(message ?? "");
            Env.Abort(p); // traps — never returns
        }

        /// <summary>
        /// Stage a custom changefeed event (operation "EVENT"), published only
        /// after a successful commit. <paramref name="payloadJson"/> must be JSON.
        /// </summary>
        public static void EmitEvent(string topic, string key, string payloadJson)
        {
            byte* t = Mem.CString(topic);
            byte* k = Mem.Bytes(key, out int klen);
            byte* v = Mem.Bytes(payloadJson, out int vlen);
            try
            {
                int rc = Env.EmitEvent(t, k, klen, v, vlen);
                if (rc < 0) throw new HostException("host_emit_event", rc);
            }
            finally
            {
                Mem.Free(t);
                Mem.Free(k);
                Mem.Free(v);
            }
        }

        /// <summary>
        /// Set the call's result payload (max 16 MiB, last call wins). The
        /// dispatcher calls this automatically for non-null reducer return
        /// values; use directly only for advanced/raw scenarios.
        /// </summary>
        public static void SetResult(string json)
        {
            byte* p = Mem.Bytes(json, out int len);
            try { Env.SetResult(p, len); }
            finally { Mem.Free(p); }
        }
    }

    /// <summary>Table operations. Values are UTF-8 JSON object strings.</summary>
    public static unsafe class Db
    {
        /// <summary>
        /// Read a row. Returns the row's JSON object string, or null if not
        /// found. Sees the current call's own staged writes first.
        /// </summary>
        public static string? Read(string table, string key)
        {
            byte* t = Mem.CString(table);
            byte* k = Mem.Bytes(key, out int klen);
            try
            {
                int outPtr = 0, outLen = 0;
                int rc = Env.TableRead(t, k, klen, &outPtr, &outLen);
                if (rc < 0) throw new HostException("host_table_read", rc);
                if (rc == 0) return null;
                return Mem.TakeGuestBuffer(outPtr, outLen);
            }
            finally
            {
                Mem.Free(t);
                Mem.Free(k);
            }
        }

        /// <summary>
        /// Stage an upsert. <paramref name="jsonValue"/> must be a JSON object
        /// (column → value). Committed atomically iff the call succeeds.
        /// </summary>
        public static void Write(string table, string key, string jsonValue)
        {
            byte* t = Mem.CString(table);
            byte* k = Mem.Bytes(key, out int klen);
            byte* v = Mem.Bytes(jsonValue, out int vlen);
            try
            {
                int rc = Env.TableWrite(t, k, klen, v, vlen);
                if (rc < 0) throw new HostException("host_table_write", rc);
            }
            finally
            {
                Mem.Free(t);
                Mem.Free(k);
                Mem.Free(v);
            }
        }

        /// <summary>Stage a delete of a row.</summary>
        public static void Delete(string table, string key)
        {
            byte* t = Mem.CString(table);
            byte* k = Mem.Bytes(key, out int klen);
            try
            {
                int rc = Env.TableDelete(t, k, klen);
                if (rc < 0) throw new HostException("host_table_delete", rc);
            }
            finally
            {
                Mem.Free(t);
                Mem.Free(k);
            }
        }

        /// <summary>
        /// Prefix scan (committed rows merged with the call's staged writes).
        /// Returns a JSON array string: [{"key": str, "value": obj}, ...],
        /// key-ordered, at most <paramref name="limit"/> rows (&lt;= 0 → 1000).
        /// </summary>
        public static string Scan(string table, string prefix = "", int limit = 0)
        {
            byte* t = Mem.CString(table);
            byte* p = Mem.Bytes(prefix, out int plen);
            try
            {
                int outPtr = 0, outLen = 0;
                int rc = Env.TableScan(t, p, plen, limit, &outPtr, &outLen);
                if (rc < 0) throw new HostException("host_table_scan", rc);
                string json = Mem.TakeGuestBuffer(outPtr, outLen);
                return json.Length > 0 ? json : "[]";
            }
            finally
            {
                Mem.Free(t);
                Mem.Free(p);
            }
        }
    }

    // =========================================================================
    // Reducer registry + dispatcher.
    // =========================================================================

    /// <summary>
    /// Registry of reducers, lifecycle hooks and subscription filters.
    /// Populate it from a [ModuleInitializer] method in your module.
    /// </summary>
    public static class Reducers
    {
        private sealed class Entry
        {
            public required Func<JsonElement[], object?> Fn;
            public string[] Params = Array.Empty<string>();
        }

        private static readonly Dictionary<string, Entry> _reducers = new(StringComparer.Ordinal);
        private static readonly Dictionary<string, Func<JsonElement, bool>> _filters = new(StringComparer.Ordinal);
        private static readonly List<string> _tables = new();
        private static string _moduleName = "module";
        private static string _moduleVersion = "1.0.0";

        /// <summary>Module name/version reported by origindb_describe.</summary>
        public static void SetModuleInfo(string name, string version = "1.0.0")
        {
            _moduleName = name;
            _moduleVersion = version;
        }

        /// <summary>Declare a table for origindb_describe metadata (informational).</summary>
        public static void DeclareTable(string table)
        {
            if (!_tables.Contains(table)) _tables.Add(table);
        }

        /// <summary>
        /// Register a reducer. <paramref name="fn"/> receives the invocation's
        /// JSON args array. A non-null return value is serialized to JSON and
        /// sent to the host via host_set_result; the call returns status 0.
        /// Throw <see cref="ReducerException"/> to fail with a specific code.
        /// Reserved lifecycle names: __init, __client_connected,
        /// __client_disconnected, __get_initial_data.
        /// </summary>
        public static void Register(string name, Func<JsonElement[], object?> fn, params string[] paramNames)
        {
            _reducers[name] = new Entry { Fn = fn, Params = paramNames ?? Array.Empty<string>() };
        }

        /// <summary>
        /// Sugar for subscription filter functions. The filter receives the
        /// changefeed event as a parsed JSON object with fields: table,
        /// operation, offset, transaction_id, key, new_value, old_value.
        /// Return true to include the event, false to exclude it
        /// (ABI status 1 / 0).
        /// </summary>
        public static void RegisterFilter(string name, Func<JsonElement, bool> filter)
        {
            _filters[name] = filter;
        }

        public static bool IsRegistered(string name) => _reducers.ContainsKey(name) || _filters.ContainsKey(name);

        // ---- dispatcher -----------------------------------------------------

        internal static int Dispatch(string name, string argsJson)
        {
            try
            {
                if (_reducers.TryGetValue(name, out var entry))
                {
                    JsonElement[] args = ParseArgs(argsJson);
                    object? result = entry.Fn(args);
                    if (result is not null)
                    {
                        Host.SetResult(SerializeResult(result));
                    }
                    return 0;
                }

                if (_filters.TryGetValue(name, out var filter))
                {
                    JsonElement[] args = ParseArgs(argsJson);
                    JsonElement ev = UnwrapEvent(args);
                    return filter(ev) ? 1 : 0;
                }

                // ABI: -404 = no handler registered for this name. The host
                // treats -404 on reserved "__" lifecycle names as a no-op.
                return -404;
            }
            catch (ReducerException rex)
            {
                Host.LogError($"reducer '{name}' failed: {rex.Message}");
                return rex.StatusCode;
            }
            catch (HostException hex)
            {
                Host.LogError($"reducer '{name}' host call failed: {hex.Message}");
                return hex.StatusCode;
            }
        }

        private static JsonElement[] ParseArgs(string argsJson)
        {
            if (string.IsNullOrWhiteSpace(argsJson)) return Array.Empty<JsonElement>();
            using JsonDocument doc = JsonDocument.Parse(argsJson);
            if (doc.RootElement.ValueKind != JsonValueKind.Array) return Array.Empty<JsonElement>();
            var args = new JsonElement[doc.RootElement.GetArrayLength()];
            int i = 0;
            foreach (JsonElement el in doc.RootElement.EnumerateArray())
            {
                args[i++] = el.Clone(); // detach from the document's lifetime
            }
            return args;
        }

        // Filters receive a single argument: the changefeed event. The host
        // passes it as a JSON *string* containing the event object, so unwrap
        // one level when possible.
        private static JsonElement UnwrapEvent(JsonElement[] args)
        {
            if (args.Length == 0) return default;
            JsonElement ev = args[0];
            if (ev.ValueKind == JsonValueKind.String)
            {
                string? inner = ev.GetString();
                if (!string.IsNullOrEmpty(inner))
                {
                    try
                    {
                        using JsonDocument doc = JsonDocument.Parse(inner);
                        return doc.RootElement.Clone();
                    }
                    catch (JsonException)
                    {
                        // Not JSON — hand the raw string element to the filter.
                    }
                }
            }
            return ev;
        }

        private static string SerializeResult(object result) => result switch
        {
            RawJson raw => raw.Json,
            JsonElement el => el.GetRawText(),
            JsonDocument doc => doc.RootElement.GetRawText(),
            bool b => b ? "true" : "false",
            _ => JsonSerializer.Serialize(result),
        };

        internal static byte[] BuildDescribeJson()
        {
            using var stream = new System.IO.MemoryStream();
            using (var w = new Utf8JsonWriter(stream))
            {
                w.WriteStartObject();
                w.WriteString("name", _moduleName);
                w.WriteString("version", _moduleVersion);
                w.WriteStartArray("reducers");
                foreach (var (name, entry) in _reducers)
                {
                    w.WriteStartObject();
                    w.WriteString("name", name);
                    w.WriteStartArray("params");
                    foreach (string p in entry.Params) w.WriteStringValue(p);
                    w.WriteEndArray();
                    w.WriteEndObject();
                }
                foreach (var name in _filters.Keys)
                {
                    w.WriteStartObject();
                    w.WriteString("name", name);
                    w.WriteStartArray("params");
                    w.WriteStringValue("event");
                    w.WriteEndArray();
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteStartArray("tables");
                foreach (string t in _tables) w.WriteStringValue(t);
                w.WriteEndArray();
                w.WriteEndObject();
            }
            return stream.ToArray();
        }
    }

    // =========================================================================
    // ABI exports — defined once for every module that compiles this file in.
    // =========================================================================
    public static unsafe class ModuleExports
    {
        private static byte* _describeBuf = null;

        /// <summary>
        /// Guest allocator used by the host for every host→guest buffer
        /// (invoke name/args, read/scan results). Buffers it returns are owned
        /// by the guest and freed with NativeMemory.Free / origindb_free.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "origindb_alloc")]
        public static nint InstantDbAlloc(int size)
        {
            try
            {
                return (nint)NativeMemory.Alloc((nuint)(size > 0 ? size : 1));
            }
            catch
            {
                return 0; // ABI: return 0 on allocation failure
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "origindb_free")]
        public static void InstantDbFree(nint ptr)
        {
            if (ptr != 0) NativeMemory.Free((void*)ptr);
        }

        /// <summary>Returns (ptr &lt;&lt; 32) | len of the module metadata JSON.</summary>
        [UnmanagedCallersOnly(EntryPoint = "origindb_describe")]
        public static long InstantDbDescribe()
        {
            try
            {
                byte[] json = Reducers.BuildDescribeJson();
                if (_describeBuf != null) NativeMemory.Free(_describeBuf);
                _describeBuf = (byte*)NativeMemory.Alloc((nuint)(json.Length == 0 ? 1 : json.Length));
                json.AsSpan().CopyTo(new Span<byte>(_describeBuf, json.Length));
                return ((long)(uint)(nint)_describeBuf << 32) | (uint)json.Length;
            }
            catch
            {
                return 0;
            }
        }

        /// <summary>
        /// Single dispatch entry point for reducers, lifecycle hooks and
        /// subscription filters. Status: 0 ok, 1/0 filter include/exclude,
        /// &lt; 0 error, -404 no handler.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "origindb_invoke")]
        public static int InstantDbInvoke(nint namePtr, int nameLen, nint argsPtr, int argsLen)
        {
            string name = "";
            try
            {
                name = nameLen > 0 ? Encoding.UTF8.GetString((byte*)namePtr, nameLen) : "";
                string argsJson = argsLen > 0 ? Encoding.UTF8.GetString((byte*)argsPtr, argsLen) : "[]";

                // The host wrote name/args via origindb_alloc; the guest owns
                // (and now frees) those buffers.
                if (namePtr != 0) NativeMemory.Free((void*)namePtr);
                if (argsPtr != 0) NativeMemory.Free((void*)argsPtr);

                return Reducers.Dispatch(name, argsJson);
            }
            catch (Exception ex)
            {
                try
                {
                    Host.LogError($"unhandled exception in origindb_invoke('{name}'): {ex}");
                }
                catch
                {
                    // ignore — never let an exception escape an export
                }
                return -1;
            }
        }
    }
}
