using System;
using System.Text.Json.Serialization;
using InstantDB;

/// <summary>
/// Counter Module Example
///
/// This example demonstrates:
/// - Defining table schemas with attributes
/// - Implementing reducers with ReducerContext
/// - Reading and writing to database tables using ctx.Db
/// - Automatic changefeed event emission (no manual Events.Emit needed)
/// - Module lifecycle management
/// </summary>
namespace CounterModule
{
    // =============================================================================
    // Data Structures
    // =============================================================================

    /// <summary>
    /// Counter table schema
    /// </summary>
    [Table(Name = "counters")]
    public class Counter
    {
        [PrimaryKey]
        [JsonPropertyName("id")]
        public string Id { get; set; } = "";

        [JsonPropertyName("value")]
        public long Value { get; set; }

        [JsonPropertyName("last_updated")]
        public ulong LastUpdated { get; set; }

        [JsonPropertyName("created_by")]
        public string CreatedBy { get; set; } = "";
    }

    /// <summary>
    /// Counter event for real-time subscriptions
    /// </summary>
    [Event(Name = "counter_events")]
    public class CounterEvent
    {
        [JsonPropertyName("counter_id")]
        public string CounterId { get; set; } = "";

        [JsonPropertyName("action")]
        public string Action { get; set; } = "";

        [JsonPropertyName("old_value")]
        public long OldValue { get; set; }

        [JsonPropertyName("new_value")]
        public long NewValue { get; set; }

        [JsonPropertyName("timestamp")]
        public ulong Timestamp { get; set; }

        [JsonPropertyName("user_id")]
        public string UserId { get; set; } = "";
    }

    /// <summary>
    /// User table schema
    /// </summary>
    [Table(Name = "users")]
    public class User
    {
        [PrimaryKey]
        [JsonPropertyName("id")]
        public ulong Id { get; set; }

        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        [JsonPropertyName("email")]
        public string Email { get; set; } = "";

        [JsonPropertyName("created_at")]
        public ulong CreatedAt { get; set; }
    }

    // =============================================================================
    // Counter Module Implementation
    // =============================================================================

    /// <summary>
    /// Main counter module class with reducers
    /// </summary>
    public class CounterModuleImpl : ModuleBase
    {
        /// <summary>
        /// Create a new counter with initial value
        /// </summary>
        [Reducer]
        public static int CreateCounter(ReducerContext ctx, string counterId, long initialValue, string createdBy)
        {
            try
            {
                // Validate inputs
                if (string.IsNullOrWhiteSpace(counterId))
                {
                    Utils.LogError("Counter ID cannot be empty");
                    return -1;
                }

                if (string.IsNullOrWhiteSpace(createdBy))
                {
                    Utils.LogError("Created by cannot be empty");
                    return -1;
                }

                // Check if counter already exists
                var existing = ctx.Db.GetTable<Counter>().Find(counterId);
                if (existing != null)
                {
                    Utils.LogWarn($"Counter already exists: {counterId}");
                    return 0; // Already exists, not an error
                }

                // Create new counter
                var counter = new Counter
                {
                    Id = counterId,
                    Value = initialValue,
                    LastUpdated = Utils.Now(),
                    CreatedBy = createdBy
                };

                // Insert into database (automatically triggers changefeed)
                ctx.Db.GetTable<Counter>().Insert(counter);

                // Note: Changefeed events are now automatically emitted by the database layer
                // No need for manual Events.Emit() calls

                Utils.LogInfo($"Created counter: {counterId} with value: {initialValue} by {createdBy}");
                return 1; // Success

            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in CreateCounter: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Increment a counter by the specified amount
        /// </summary>
        [Reducer]
        public static int IncrementCounter(ReducerContext ctx, string counterId, long amount, string userId)
        {
            try
            {
                // Validate inputs
                if (string.IsNullOrWhiteSpace(counterId))
                {
                    Utils.LogError("Counter ID cannot be empty");
                    return -1;
                }

                // Read current counter
                var current = ctx.Db.GetTable<Counter>().Find(counterId);
                if (current == null)
                {
                    Utils.LogError($"Counter not found: {counterId}");
                    return -2; // Not found
                }

                long oldValue = current.Value;

                // Update counter
                current.Value += amount;
                current.LastUpdated = Utils.Now();

                // Update in database (automatically triggers changefeed)
                ctx.Db.GetTable<Counter>().Update(current);

                // Note: Changefeed events are now automatically emitted by the database layer

                Utils.LogInfo($"Incremented counter: {counterId} by {amount} " +
                             $"(old: {oldValue}, new: {current.Value})");

                return 1; // Success

            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in IncrementCounter: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Set counter to a specific value
        /// </summary>
        [Reducer]
        public static int SetCounterValue(ReducerContext ctx, string counterId, long newValue, string userId)
        {
            try
            {
                // Read current counter
                var current = ctx.Db.GetTable<Counter>().Find(counterId);
                if (current == null)
                {
                    Utils.LogError($"Counter not found: {counterId}");
                    return -2; // Not found
                }

                long oldValue = current.Value;

                // Update counter
                current.Value = newValue;
                current.LastUpdated = Utils.Now();

                // Update in database (automatically triggers changefeed)
                ctx.Db.GetTable<Counter>().Update(current);

                // Note: Changefeed events are now automatically emitted by the database layer

                Utils.LogInfo($"Set counter: {counterId} to {newValue} " +
                             $"(old: {oldValue}, new: {newValue})");

                return 1; // Success

            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in SetCounterValue: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Get current counter value
        /// </summary>
        [Reducer]
        public static long GetCounterValue(ReducerContext ctx, string counterId)
        {
            try
            {
                var counter = ctx.Db.GetTable<Counter>().Find(counterId);
                if (counter == null)
                {
                    Utils.LogWarn($"Counter not found: {counterId}");
                    return 0; // Return 0 for not found
                }

                return counter.Value;

            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in GetCounterValue: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Delete a counter
        /// </summary>
        [Reducer]
        public static int DeleteCounter(ReducerContext ctx, string counterId, string userId)
        {
            try
            {
                // Read current value to check if exists
                var current = ctx.Db.GetTable<Counter>().Find(counterId);
                if (current == null)
                {
                    return 0; // Already deleted or never existed
                }

                // Delete the counter (automatically triggers changefeed)
                bool wasDeleted = ctx.Db.GetTable<Counter>().Delete(counterId);
                if (wasDeleted)
                {
                    Utils.LogInfo($"Deleted counter: {counterId}");
                }

                // Note: Changefeed events are now automatically emitted by the database layer
                return wasDeleted ? 1 : 0;

            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in DeleteCounter: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Create a new user (example of working with multiple tables)
        /// </summary>
        [Reducer]
        public static int CreateUser(ReducerContext ctx, string name, string email)
        {
            try
            {
                // Validate inputs
                if (string.IsNullOrWhiteSpace(name))
                {
                    Utils.LogError("Name cannot be empty");
                    return -1;
                }

                if (string.IsNullOrWhiteSpace(email))
                {
                    Utils.LogError("Email cannot be empty");
                    return -1;
                }

                // Create new user
                var user = new User
                {
                    Id = Utils.GenerateId(),
                    Name = name,
                    Email = email,
                    CreatedAt = Utils.Now()
                };

                // Insert into database (automatically triggers changefeed)
                ctx.Db.GetTable<User>().Insert(user);

                Utils.LogInfo($"Created user: {user.Id} ({name}, {email})");
                return (int)user.Id; // Return user ID

            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in CreateUser: {ex.Message}");
                return -1;
            }
        }

        // =============================================================================
        // Module Lifecycle Overrides
        // =============================================================================

        public override int Initialize()
        {
            Utils.LogInfo("Counter module initialized successfully");

            // Note: In the new API, initialization would use a system context
            // For now, initialization is simpler without complex database operations

            return 0;
        }

        public override int OnClientConnected(string connectionId)
        {
            Utils.LogInfo($"Client connected to counter module: {connectionId}");

            // Note: Client lifecycle events could trigger reducer calls with system context
            // For now, keep these simple without database operations

            return 0;
        }

        public override int OnClientDisconnected(string connectionId)
        {
            Utils.LogInfo($"Client disconnected from counter module: {connectionId}");

            // Note: Client lifecycle events could trigger reducer calls with system context
            // For now, keep these simple without database operations

            return 0;
        }
    }
}