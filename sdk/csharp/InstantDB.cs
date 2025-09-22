using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

/// <summary>
/// InstantDB C# SDK for WebAssembly Modules
///
/// This SDK provides a C# interface for writing WASM modules that run inside
/// InstantDB.
///
/// Key Features:
/// - Type-safe database operations
/// - Automatic JSON serialization/deserialization
/// - Event emission for real-time subscriptions
/// - Attribute-based reducer definitions
/// - Table schema definitions
/// </summary>
namespace InstantDB
{
    // =============================================================================
    // Core Types
    // =============================================================================

    /// <summary>
    /// Primary key type - can be string or integer
    /// </summary>
    public readonly struct Key
    {
        private readonly object _value;

        public Key(string value) => _value = value;
        public Key(long value) => _value = value;
        public Key(ulong value) => _value = value;
        public Key(int value) => _value = (long)value;
        public Key(uint value) => _value = (ulong)value;

        public byte[] ToBytes()
        {
            return _value switch
            {
                string s => Encoding.UTF8.GetBytes(s),
                long l => BitConverter.GetBytes(l),
                ulong ul => BitConverter.GetBytes(ul),
                _ => throw new ArgumentException("Unsupported key type")
            };
        }

        public override string ToString() => _value?.ToString() ?? "";
    }

    /// <summary>
    /// Result type for operations that can fail
    /// </summary>
    public readonly struct Result<T>
    {
        private readonly T? _value;
        private readonly string? _error;
        public readonly bool IsOk;

        public Result(T value)
        {
            _value = value;
            _error = null;
            IsOk = true;
        }

        public Result(string error)
        {
            _value = default;
            _error = error;
            IsOk = false;
        }

        public bool IsErr => !IsOk;

        public T Unwrap()
        {
            if (!IsOk)
                throw new InvalidOperationException($"Called Unwrap() on error result: {_error}");
            return _value!;
        }

        public T UnwrapOr(T defaultValue) => IsOk ? _value! : defaultValue;

        public string Error()
        {
            if (IsOk)
                throw new InvalidOperationException("Called Error() on ok result");
            return _error!;
        }

        public static implicit operator Result<T>(T value) => new(value);
        public static implicit operator Result<T>(string error) => new(error);
    }

    /// <summary>
    /// Represents an empty success result
    /// </summary>
    public readonly struct Unit
    {
        public static readonly Unit Value = new();
    }

    // =============================================================================
    // Attributes for Code Generation
    // =============================================================================

    /// <summary>
    /// Reducer kinds
    /// </summary>
    public enum ReducerKind
    {
        Update,
        Init
    }

    /// <summary>
    /// Marks a method as a reducer function
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class ReducerAttribute : Attribute
    {
        public string? Name { get; set; }
        public ReducerKind Kind { get; set; } = ReducerKind.Update;

        public ReducerAttribute() { }
        public ReducerAttribute(ReducerKind kind) { Kind = kind; }
    }

    /// <summary>
    /// Marks a class as a table schema
    /// </summary>
    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
    public class TableAttribute : Attribute
    {
        public string? Name { get; set; }
    }

    /// <summary>
    /// Marks a property as the primary key
    /// </summary>
    [AttributeUsage(AttributeTargets.Property)]
    public class PrimaryKeyAttribute : Attribute { }

    /// <summary>
    /// Marks a property as a foreign key
    /// </summary>
    [AttributeUsage(AttributeTargets.Property)]
    public class ForeignKeyAttribute : Attribute
    {
        public Type ReferencedTable { get; }
        public ForeignKeyAttribute(Type referencedTable) => ReferencedTable = referencedTable;
    }

    /// <summary>
    /// Marks a class as an event schema
    /// </summary>
    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
    public class EventAttribute : Attribute
    {
        public string? Name { get; set; }
    }

    /// <summary>
    /// Marks a method as module initialization handler
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class InitAttribute : Attribute { }

    /// <summary>
    /// Marks a method as client connection handler
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class ClientConnectedAttribute : Attribute { }

    /// <summary>
    /// Marks a method as client disconnection handler
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class ClientDisconnectedAttribute : Attribute { }

    // =============================================================================
    // Host API - Low-level interface to InstantDB
    // =============================================================================

    /// <summary>
    /// Low-level host functions imported from InstantDB WASM host
    /// </summary>
    public static class HostApi
    {
        // Database operations
        [DllImport("*", EntryPoint = "host_table_read")]
        private static extern int HostTableRead(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string table,
            IntPtr key, int keyLen,
            out IntPtr outPtr, out int outLen);

        [DllImport("*", EntryPoint = "host_table_write")]
        private static extern int HostTableWrite(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string table,
            IntPtr key, int keyLen,
            IntPtr value, int valueLen);

        [DllImport("*", EntryPoint = "host_table_delete")]
        private static extern int HostTableDelete(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string table,
            IntPtr key, int keyLen);

        [DllImport("*", EntryPoint = "host_table_scan")]
        private static extern int HostTableScan(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string table,
            IntPtr prefix, int prefixLen,
            int limit,
            out IntPtr outPtr, out int outLen);

        // Event emission
        [DllImport("*", EntryPoint = "host_emit_event")]
        private static extern int HostEmitEvent(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string topic,
            IntPtr key, int keyLen,
            IntPtr payload, int payloadLen);

        // Utility functions
        [DllImport("*", EntryPoint = "host_now_ms")]
        public static extern ulong HostNowMs();

        [DllImport("*", EntryPoint = "host_generate_id")]
        public static extern ulong HostGenerateId();

        [DllImport("*", EntryPoint = "host_log")]
        public static extern void HostLog(int level, [MarshalAs(UnmanagedType.LPUTF8Str)] string message);

        [DllImport("*", EntryPoint = "host_abort")]
        public static extern void HostAbort([MarshalAs(UnmanagedType.LPUTF8Str)] string message);

        // Memory management
        [DllImport("*", EntryPoint = "host_alloc")]
        public static extern IntPtr HostAlloc(int size);

        [DllImport("*", EntryPoint = "host_free")]
        public static extern void HostFree(IntPtr ptr);

        // High-level wrapper methods
        public static Result<T?> TableRead<T>(string table, Key key) where T : class
        {
            var keyBytes = key.ToBytes();
            IntPtr keyPtr = Marshal.AllocHGlobal(keyBytes.Length);
            try
            {
                Marshal.Copy(keyBytes, 0, keyPtr, keyBytes.Length);

                int result = HostTableRead(table, keyPtr, keyBytes.Length, out IntPtr outPtr, out int outLen);

                if (result == 0)
                {
                    return (T?)null; // Not found
                }
                else if (result < 0)
                {
                    return "Database read failed";
                }

                try
                {
                    byte[] data = new byte[outLen];
                    Marshal.Copy(outPtr, data, 0, outLen);
                    HostFree(outPtr);

                    string json = Encoding.UTF8.GetString(data);
                    T? obj = JsonSerializer.Deserialize<T>(json);
                    return obj;
                }
                catch (JsonException ex)
                {
                    return $"Deserialization failed: {ex.Message}";
                }
            }
            finally
            {
                Marshal.FreeHGlobal(keyPtr);
            }
        }

        public static Result<Unit> TableWrite<T>(string table, Key key, T value)
        {
            var keyBytes = key.ToBytes();
            string json = JsonSerializer.Serialize(value);
            var valueBytes = Encoding.UTF8.GetBytes(json);

            IntPtr keyPtr = Marshal.AllocHGlobal(keyBytes.Length);
            IntPtr valuePtr = Marshal.AllocHGlobal(valueBytes.Length);

            try
            {
                Marshal.Copy(keyBytes, 0, keyPtr, keyBytes.Length);
                Marshal.Copy(valueBytes, 0, valuePtr, valueBytes.Length);

                int result = HostTableWrite(table, keyPtr, keyBytes.Length, valuePtr, valueBytes.Length);

                if (result < 0)
                {
                    return "Database write failed";
                }

                return Unit.Value;
            }
            finally
            {
                Marshal.FreeHGlobal(keyPtr);
                Marshal.FreeHGlobal(valuePtr);
            }
        }

        public static Result<bool> TableDelete(string table, Key key)
        {
            var keyBytes = key.ToBytes();
            IntPtr keyPtr = Marshal.AllocHGlobal(keyBytes.Length);

            try
            {
                Marshal.Copy(keyBytes, 0, keyPtr, keyBytes.Length);

                int result = HostTableDelete(table, keyPtr, keyBytes.Length);

                if (result < 0)
                {
                    return "Database delete failed";
                }

                return result > 0;
            }
            finally
            {
                Marshal.FreeHGlobal(keyPtr);
            }
        }

        public static Result<Unit> EmitEvent<T>(string topic, string key, T payload)
        {
            string json = JsonSerializer.Serialize(payload);
            var keyBytes = Encoding.UTF8.GetBytes(key);
            var payloadBytes = Encoding.UTF8.GetBytes(json);

            IntPtr keyPtr = Marshal.AllocHGlobal(keyBytes.Length);
            IntPtr payloadPtr = Marshal.AllocHGlobal(payloadBytes.Length);

            try
            {
                Marshal.Copy(keyBytes, 0, keyPtr, keyBytes.Length);
                Marshal.Copy(payloadBytes, 0, payloadPtr, payloadBytes.Length);

                int result = HostEmitEvent(topic, keyPtr, keyBytes.Length, payloadPtr, payloadBytes.Length);

                if (result < 0)
                {
                    return "Event emission failed";
                }

                return Unit.Value;
            }
            finally
            {
                Marshal.FreeHGlobal(keyPtr);
                Marshal.FreeHGlobal(payloadPtr);
            }
        }
    }

    // =============================================================================
    // ReducerContext
    // =============================================================================

    /// <summary>
    /// Context provided to reducer functions
    /// </summary>
    public class ReducerContext
    {
        /// <summary>
        /// The identity of the caller (client ID)
        /// </summary>
        public string Sender { get; internal set; } = "";

        /// <summary>
        /// Random number generator for deterministic randomness
        /// </summary>
        public Random Random { get; internal set; } = new Random();

        /// <summary>
        /// Database access through table collections
        /// </summary>
        public DbContext Db { get; internal set; } = new DbContext();
    }

    /// <summary>
    /// Database context with table access
    /// </summary>
    public class DbContext
    {
        private readonly Dictionary<Type, object> _tables = new();
        private readonly Dictionary<string, object> _dynamicTables = new();

        /// <summary>
        /// Get table accessor for a specific type
        /// </summary>
        public Table<T> GetTable<T>() where T : class, new()
        {
            var type = typeof(T);
            if (!_tables.ContainsKey(type))
            {
                _tables[type] = new Table<T>();
            }
            return (Table<T>)_tables[type];
        }

        /// <summary>
        /// Dynamic table access - allows ctx.Db.config, ctx.Db.server_state, etc.
        /// </summary>
        public dynamic this[string tableName]
        {
            get
            {
                if (!_dynamicTables.ContainsKey(tableName))
                {
                    _dynamicTables[tableName] = new DynamicTable(tableName);
                }
                return _dynamicTables[tableName];
            }
        }

        // Support for property-style access like ctx.Db.config
        public DynamicTable config => (DynamicTable)this["config"];
        public DynamicTable server_state => (DynamicTable)this["server_state"];
        public DynamicTable users => (DynamicTable)this["users"];
        public DynamicTable user_activities => (DynamicTable)this["user_activities"];
    }

    /// <summary>
    /// Dynamic table accessor for untyped table operations
    /// </summary>
    public class DynamicTable
    {
        private readonly string _tableName;

        public DynamicTable(string tableName)
        {
            _tableName = tableName;
        }

        /// <summary>
        /// Insert a row using dynamic object (automatically triggers changefeed)
        /// </summary>
        public void Insert<T>(T row)
        {
            var key = GetPrimaryKeyFromObject(row);
            var result = DB.Write(_tableName, key, row);
            if (result.IsErr)
            {
                throw new DatabaseException($"Failed to insert into {_tableName}: {result.Error()}");
            }
            // Changefeed events are automatically emitted by the database layer
        }

        /// <summary>
        /// Update a row using dynamic object (automatically triggers changefeed)
        /// </summary>
        public void Update<T>(T row)
        {
            Insert(row); // Same operation - upsert behavior
        }

        /// <summary>
        /// Delete a row by primary key (automatically triggers changefeed)
        /// </summary>
        public bool Delete(object primaryKey)
        {
            var key = new Key(primaryKey);
            var result = DB.Delete(_tableName, key);
            if (result.IsErr)
            {
                throw new DatabaseException($"Failed to delete from {_tableName}: {result.Error()}");
            }
            return result.Unwrap();
        }

        /// <summary>
        /// Find a row by primary key
        /// </summary>
        public T? Find<T>(object primaryKey) where T : class
        {
            var key = new Key(primaryKey);
            var result = DB.Read<T>(_tableName, key);
            if (result.IsErr)
            {
                throw new DatabaseException($"Failed to read from {_tableName}: {result.Error()}");
            }
            return result.Unwrap();
        }

        private Key GetPrimaryKeyFromObject<T>(T obj)
        {
            if (obj == null) throw new ArgumentNullException(nameof(obj));

            var type = obj.GetType();
            var properties = type.GetProperties();

            foreach (var prop in properties)
            {
                if (Attribute.GetCustomAttribute(prop, typeof(PrimaryKeyAttribute)) != null)
                {
                    var value = prop.GetValue(obj);
                    if (value == null)
                    {
                        throw new InvalidOperationException($"Primary key {prop.Name} cannot be null");
                    }
                    return new Key(value);
                }
            }

            // Try common primary key property names if no attribute found
            var idProp = properties.FirstOrDefault(p =>
                p.Name.Equals("Id", StringComparison.OrdinalIgnoreCase) ||
                p.Name.Equals("ID", StringComparison.OrdinalIgnoreCase));

            if (idProp != null)
            {
                var value = idProp.GetValue(obj);
                if (value != null)
                {
                    return new Key(value);
                }
            }

            throw new InvalidOperationException($"No primary key found for object of type {type.Name}");
        }
    }

    /// <summary>
    /// Table accessor that automatically handles change detection
    /// </summary>
    public class Table<T> where T : class, new()
    {
        private readonly string _tableName;

        public Table()
        {
            var type = typeof(T);
            var attr = (TableAttribute?)Attribute.GetCustomAttribute(type, typeof(TableAttribute));
            _tableName = attr?.Name ?? type.Name.ToLower();
        }

        /// <summary>
        /// Insert a new row (automatically triggers changefeed)
        /// </summary>
        public void Insert(T row)
        {
            var key = GetPrimaryKey(row);
            var result = DB.Write(_tableName, key, row);
            if (result.IsErr)
            {
                throw new DatabaseException($"Failed to insert into {_tableName}: {result.Error()}");
            }
            // Changefeed events are automatically emitted by the database layer
        }

        /// <summary>
        /// Update an existing row (automatically triggers changefeed)
        /// </summary>
        public void Update(T row)
        {
            Insert(row); // Same operation - upsert behavior
        }

        /// <summary>
        /// Delete a row by primary key (automatically triggers changefeed)
        /// </summary>
        public bool Delete(object primaryKey)
        {
            var key = new Key(primaryKey);
            var result = DB.Delete(_tableName, key);
            if (result.IsErr)
            {
                throw new DatabaseException($"Failed to delete from {_tableName}: {result.Error()}");
            }
            return result.Unwrap();
        }

        /// <summary>
        /// Find a row by primary key
        /// </summary>
        public T? Find(object primaryKey)
        {
            var key = new Key(primaryKey);
            var result = DB.Read<T>(_tableName, key);
            if (result.IsErr)
            {
                throw new DatabaseException($"Failed to read from {_tableName}: {result.Error()}");
            }
            return result.Unwrap();
        }

        /// <summary>
        /// Get all rows (simple scan - would be optimized in production)
        /// </summary>
        public IEnumerable<T> All()
        {
            // This would need a scan operation in the real implementation
            // For now, return empty collection
            return new List<T>();
        }

        private Key GetPrimaryKey(T row)
        {
            var type = typeof(T);
            var properties = type.GetProperties();

            foreach (var prop in properties)
            {
                if (Attribute.GetCustomAttribute(prop, typeof(PrimaryKeyAttribute)) != null)
                {
                    var value = prop.GetValue(row);
                    if (value == null)
                    {
                        throw new InvalidOperationException($"Primary key {prop.Name} cannot be null");
                    }
                    return new Key(value);
                }
            }

            throw new InvalidOperationException($"No primary key found for type {type.Name}");
        }
    }

    // =============================================================================
    // High-level Database Operations
    // =============================================================================

    /// <summary>
    /// High-level database operations with type safety
    /// </summary>
    public static class DB
    {
        /// <summary>
        /// Read a value from a table
        /// </summary>
        public static Result<T?> Read<T>(string table, Key key) where T : class
        {
            return HostApi.TableRead<T>(table, key);
        }

        /// <summary>
        /// Write a value to a table
        /// </summary>
        public static Result<Unit> Write<T>(string table, Key key, T value)
        {
            return HostApi.TableWrite(table, key, value);
        }

        /// <summary>
        /// Delete a value from a table
        /// </summary>
        public static Result<bool> Delete(string table, Key key)
        {
            return HostApi.TableDelete(table, key);
        }

        /// <summary>
        /// Read using the table name from attribute
        /// </summary>
        public static Result<T?> Read<T>(Key key) where T : class
        {
            string tableName = GetTableName<T>();
            return Read<T>(tableName, key);
        }

        /// <summary>
        /// Write using the table name from attribute
        /// </summary>
        public static Result<Unit> Write<T>(Key key, T value)
        {
            string tableName = GetTableName<T>();
            return Write(tableName, key, value);
        }

        /// <summary>
        /// Delete using the table name from attribute
        /// </summary>
        public static Result<bool> Delete<T>(Key key) where T : class
        {
            string tableName = GetTableName<T>();
            return Delete(tableName, key);
        }

        private static string GetTableName<T>()
        {
            var type = typeof(T);
            var attr = (TableAttribute?)Attribute.GetCustomAttribute(type, typeof(TableAttribute));
            return attr?.Name ?? type.Name.ToLower();
        }
    }

    // =============================================================================
    // Event System
    // =============================================================================

    /// <summary>
    /// Event emission for real-time subscriptions
    /// </summary>
    public static class Events
    {
        /// <summary>
        /// Emit an event to the changefeed system
        /// </summary>
        public static Result<Unit> Emit<T>(string topic, string key, T payload)
        {
            return HostApi.EmitEvent(topic, key, payload);
        }

        /// <summary>
        /// Emit an event using the event name from attribute
        /// </summary>
        public static Result<Unit> Emit<T>(string key, T payload)
        {
            string eventName = GetEventName<T>();
            return Emit(eventName, key, payload);
        }

        private static string GetEventName<T>()
        {
            var type = typeof(T);
            var attr = (EventAttribute?)Attribute.GetCustomAttribute(type, typeof(EventAttribute));
            return attr?.Name ?? type.Name.ToLower();
        }
    }

    // =============================================================================
    // Utility Functions
    // =============================================================================

    /// <summary>
    /// Utility functions for common operations
    /// </summary>
    public static class Utils
    {
        /// <summary>
        /// Log levels for debugging
        /// </summary>
        public enum LogLevel
        {
            Trace = 0,
            Debug = 1,
            Info = 2,
            Warn = 3,
            Error = 4
        }

        /// <summary>
        /// Get current timestamp in milliseconds
        /// </summary>
        public static ulong Now() => HostApi.HostNowMs();

        /// <summary>
        /// Generate a unique ID
        /// </summary>
        public static ulong GenerateId() => HostApi.HostGenerateId();

        /// <summary>
        /// Log a message with the specified level
        /// </summary>
        public static void Log(LogLevel level, string message)
        {
            HostApi.HostLog((int)level, message);
        }

        /// <summary>
        /// Abort the current transaction with an error message
        /// </summary>
        public static void Abort(string message)
        {
            HostApi.HostAbort(message);
        }

        /// <summary>
        /// Log with different levels (convenience methods)
        /// </summary>
        public static void LogTrace(string message) => Log(LogLevel.Trace, message);
        public static void LogDebug(string message) => Log(LogLevel.Debug, message);
        public static void LogInfo(string message) => Log(LogLevel.Info, message);
        public static void LogWarn(string message) => Log(LogLevel.Warn, message);
        public static void LogError(string message) => Log(LogLevel.Error, message);
    }

    // =============================================================================
    // Module Lifecycle
    // =============================================================================

    // =============================================================================
    // Subscription System
    // =============================================================================

    /// <summary>
    /// Subscription filter function attribute
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class SubscriptionFilterAttribute : Attribute
    {
        public string? Name { get; set; }
    }

    /// <summary>
    /// Subscription transform function attribute
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class SubscriptionTransformAttribute : Attribute
    {
        public string? Name { get; set; }
    }

    /// <summary>
    /// Initial data function attribute for subscriptions
    /// </summary>
    [AttributeUsage(AttributeTargets.Method)]
    public class InitialDataAttribute : Attribute
    {
        public string? Name { get; set; }
    }

    /// <summary>
    /// Subscription utilities for WASM modules
    /// </summary>
    public static class Subscriptions
    {
        /// <summary>
        /// Filter function for subscription events
        /// Override this in your module to implement custom filtering
        /// </summary>
        [SubscriptionFilter]
        public static bool FilterEvent(byte[] eventData)
        {
            // Default implementation - include all events
            return true;
        }

        /// <summary>
        /// Transform function for subscription events
        /// Override this in your module to implement custom transformations
        /// </summary>
        [SubscriptionTransform]
        public static byte[] TransformEvent(byte[] eventData)
        {
            // Default implementation - pass through unchanged
            return eventData;
        }

        /// <summary>
        /// Get initial data for subscription
        /// Called when client subscribes with include_initial_data = true
        /// </summary>
        [InitialData]
        public static byte[] GetInitialData(string whereClause)
        {
            // Default implementation - return empty
            return Array.Empty<byte>();
        }

        /// <summary>
        /// Emit event to specific client
        /// </summary>
        public static void EmitToClient(string clientId, string eventType, byte[] data)
        {
            // Implementation would use host API to emit to specific client
            // This is a placeholder for the host function call
        }
    }

    /// <summary>
    /// Base class for InstantDB modules
    /// </summary>
    public abstract class ModuleBase
    {
        /// <summary>
        /// Called when the module is first loaded
        /// </summary>
        [Init]
        public virtual int Initialize()
        {
            Utils.LogInfo($"{GetType().Name} module initialized");
            return 0;
        }

        /// <summary>
        /// Called when a WebSocket client connects
        /// </summary>
        [ClientConnected]
        public virtual int OnClientConnected(string connectionId)
        {
            Utils.LogInfo($"Client connected: {connectionId}");
            return 0;
        }

        /// <summary>
        /// Called when a WebSocket client disconnects
        /// </summary>
        [ClientDisconnected]
        public virtual int OnClientDisconnected(string connectionId)
        {
            Utils.LogInfo($"Client disconnected: {connectionId}");
            return 0;
        }
    }

    // =============================================================================
    // Exception Types
    // =============================================================================

    /// <summary>
    /// Exception thrown when a database operation fails
    /// </summary>
    public class DatabaseException : Exception
    {
        public DatabaseException(string message) : base(message) { }
        public DatabaseException(string message, Exception innerException) : base(message, innerException) { }
    }

    /// <summary>
    /// Exception thrown when a reducer operation fails
    /// </summary>
    public class ReducerException : Exception
    {
        public ReducerException(string message) : base(message) { }
        public ReducerException(string message, Exception innerException) : base(message, innerException) { }
    }
}