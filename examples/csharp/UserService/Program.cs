// =============================================================================
// UserService — example OriginDB WASM module in C#.
//
// Reducers:
//   CreateUser(name, email) -> {"id": "..."}
//   GetUsers()              -> [ {user row}, ... ]
// Filter:
//   OnlyUserInserts         -> forwards only INSERT events on the users table
//
// Build:  dotnet publish -c Release
// Output: bin/Release/net8.0/wasi-wasm/AppBundle/UserService.wasm
// =============================================================================

#nullable enable

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text.Json;
using OriginDB;

namespace UserService;

public static class Program
{
    // WASI command entry point (required by OutputType=Exe). Left empty on
    // purpose — the host may never call _start, so registration lives in the
    // [ModuleInitializer] below.
    public static void Main() { }

    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("user_service", "1.0.0");
        Reducers.DeclareTable("users");

        Reducers.Register("CreateUser", CreateUser, "name", "email");
        Reducers.Register("GetUsers", GetUsers);
        Reducers.RegisterFilter("OnlyUserInserts", OnlyUserInserts);

        Reducers.Register("__init", _ =>
        {
            Host.LogInfo("user_service module initialized");
            return null;
        });
    }

    /// <summary>CreateUser(name, email) — validates, writes a users row, returns {"id": id}.</summary>
    private static object? CreateUser(JsonElement[] args)
    {
        string name = RequireString(args, 0, "name");
        string email = RequireString(args, 1, "email");

        if (name.Length > 256)
            throw new ReducerException("name must be at most 256 characters", -3);
        int at = email.IndexOf('@');
        if (at <= 0 || at == email.Length - 1)
            throw new ReducerException($"'{email}' is not a valid email address", -3);

        long id = Host.GenerateId();
        string key = id.ToString();

        var row = new Dictionary<string, object?>
        {
            ["id"] = key,
            ["name"] = name,
            ["email"] = email,
            ["created_at"] = Host.NowMs(),
        };
        Db.Write("users", key, JsonSerializer.Serialize(row));

        Host.LogInfo($"created user {key} ({name})");
        return new Dictionary<string, object?> { ["id"] = key };
    }

    /// <summary>GetUsers() — scans the users table and returns an array of rows.</summary>
    private static object? GetUsers(JsonElement[] args)
    {
        // Db.Scan returns [{"key": str, "value": obj}, ...]; project the rows.
        using JsonDocument doc = JsonDocument.Parse(Db.Scan("users"));
        var users = new List<JsonElement>();
        foreach (JsonElement entry in doc.RootElement.EnumerateArray())
        {
            if (entry.TryGetProperty("value", out JsonElement value))
                users.Add(value.Clone());
        }
        return users;
    }

    /// <summary>
    /// Subscription filter: include only INSERT events on the users table.
    /// Register it on a subscription by name ("OnlyUserInserts").
    /// </summary>
    private static bool OnlyUserInserts(JsonElement ev)
    {
        return ev.ValueKind == JsonValueKind.Object
            && ev.TryGetProperty("table", out JsonElement table)
            && table.GetString() == "users"
            && ev.TryGetProperty("operation", out JsonElement op)
            && op.GetString() == "INSERT";
    }

    private static string RequireString(JsonElement[] args, int index, string name)
    {
        if (args.Length <= index || args[index].ValueKind != JsonValueKind.String)
            throw new ReducerException($"argument {index} ('{name}') must be a string", -3);
        string value = (args[index].GetString() ?? "").Trim();
        if (value.Length == 0)
            throw new ReducerException($"argument '{name}' must not be empty", -3);
        return value;
    }
}
