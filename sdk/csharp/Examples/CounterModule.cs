// =============================================================================
// CounterModule — reference example for the InstantDB C# SDK.
//
// Demonstrates:
//   * Registering reducers with Reducers.Register from a [ModuleInitializer]
//   * Table read/write/delete/scan with JSON string values
//   * Returning results (serialized via host_set_result by the SDK)
//   * Failing a call with ReducerException (negative ABI status)
//   * A subscription filter registered with Reducers.RegisterFilter
//
// Build (from a project that includes this file + InstantDB.cs):
//   dotnet publish -c Release
//   → bin/Release/net8.0/wasi-wasm/AppBundle/<Project>.wasm
// =============================================================================

#nullable enable

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text.Json;
using InstantDB;

namespace CounterModule
{
    public static class Program
    {
        // WASI command entry point (required by OutputType=Exe). Registration
        // does not happen here — the host may never call _start.
        public static void Main() { }

        // Runs before any export is dispatched, so the registry is always
        // populated before the host's first instantdb_invoke/describe call.
        [ModuleInitializer]
        internal static void Init()
        {
            Reducers.SetModuleInfo("counter", "1.0.0");
            Reducers.DeclareTable("counters");

            Reducers.Register("CreateCounter", CreateCounter, "counterId", "initialValue");
            Reducers.Register("Increment", Increment, "counterId", "amount");
            Reducers.Register("GetCounter", GetCounter, "counterId");
            Reducers.Register("DeleteCounter", DeleteCounter, "counterId");
            Reducers.Register("ListCounters", ListCounters);

            // Lifecycle hooks are ordinary registrations under reserved names.
            Reducers.Register("__init", _ =>
            {
                Host.LogInfo("counter module initialized");
                return null;
            });

            // Subscription filter: only forward changefeed events whose new
            // value is >= 10. Event fields: table, operation, offset,
            // transaction_id, key, new_value, old_value.
            Reducers.RegisterFilter("OnlyBigValues", ev =>
            {
                if (ev.ValueKind != JsonValueKind.Object) return true;
                if (ev.TryGetProperty("new_value", out JsonElement nv) &&
                    nv.ValueKind == JsonValueKind.Object &&
                    nv.TryGetProperty("value", out JsonElement v) &&
                    v.ValueKind == JsonValueKind.Number)
                {
                    return v.GetInt64() >= 10;
                }
                return true;
            });
        }

        private static object? CreateCounter(JsonElement[] args)
        {
            string counterId = GetStringArg(args, 0, "counterId");
            long initialValue = args.Length > 1 && args[1].ValueKind == JsonValueKind.Number
                ? args[1].GetInt64()
                : 0;

            if (Db.Read("counters", counterId) is not null)
                throw new ReducerException($"counter '{counterId}' already exists", -3);

            var row = new Dictionary<string, object?>
            {
                ["id"] = counterId,
                ["value"] = initialValue,
                ["updated_at"] = Host.NowMs(),
            };
            Db.Write("counters", counterId, JsonSerializer.Serialize(row));

            Host.LogInfo($"created counter '{counterId}' = {initialValue}");
            return new Dictionary<string, object?> { ["id"] = counterId, ["value"] = initialValue };
        }

        private static object? Increment(JsonElement[] args)
        {
            string counterId = GetStringArg(args, 0, "counterId");
            long amount = args.Length > 1 && args[1].ValueKind == JsonValueKind.Number
                ? args[1].GetInt64()
                : 1;

            string? json = Db.Read("counters", counterId)
                ?? throw new ReducerException($"counter '{counterId}' not found", -3);

            using JsonDocument doc = JsonDocument.Parse(json);
            long value = doc.RootElement.TryGetProperty("value", out JsonElement v) &&
                         v.ValueKind == JsonValueKind.Number
                ? v.GetInt64()
                : 0;

            long newValue = value + amount;
            var row = new Dictionary<string, object?>
            {
                ["id"] = counterId,
                ["value"] = newValue,
                ["updated_at"] = Host.NowMs(),
            };
            Db.Write("counters", counterId, JsonSerializer.Serialize(row));

            // Custom changefeed event on top of the automatic table change event.
            Host.EmitEvent("counter_events", counterId, JsonSerializer.Serialize(
                new Dictionary<string, object?>
                {
                    ["counter_id"] = counterId,
                    ["old_value"] = value,
                    ["new_value"] = newValue,
                }));

            return new Dictionary<string, object?> { ["id"] = counterId, ["value"] = newValue };
        }

        private static object? GetCounter(JsonElement[] args)
        {
            string counterId = GetStringArg(args, 0, "counterId");
            string? json = Db.Read("counters", counterId);
            // RawJson passes an existing JSON document through verbatim.
            return json is null ? new RawJson("null") : new RawJson(json);
        }

        private static object? DeleteCounter(JsonElement[] args)
        {
            string counterId = GetStringArg(args, 0, "counterId");
            Db.Delete("counters", counterId);
            return new Dictionary<string, object?> { ["deleted"] = counterId };
        }

        private static object? ListCounters(JsonElement[] args)
        {
            // Scan returns [{"key": str, "value": obj}, ...]; project the values.
            using JsonDocument doc = JsonDocument.Parse(Db.Scan("counters"));
            var counters = new List<JsonElement>();
            foreach (JsonElement row in doc.RootElement.EnumerateArray())
            {
                if (row.TryGetProperty("value", out JsonElement value))
                    counters.Add(value.Clone());
            }
            return counters;
        }

        private static string GetStringArg(JsonElement[] args, int index, string name)
        {
            if (args.Length <= index || args[index].ValueKind != JsonValueKind.String)
                throw new ReducerException($"argument {index} ('{name}') must be a string", -3);
            string value = args[index].GetString() ?? "";
            if (value.Length == 0)
                throw new ReducerException($"argument '{name}' must not be empty", -3);
            return value;
        }
    }
}
