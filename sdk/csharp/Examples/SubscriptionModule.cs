using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;
using InstantDB;

/// <summary>
/// Subscription Module Example
///
/// This example demonstrates:
/// - Implementing subscription filter functions
/// - Transforming subscription events
/// - Providing initial data for subscriptions
/// - Client-specific event handling
/// - Using both context-based (reducers) and global (subscriptions) database access
/// </summary>
namespace SubscriptionModule
{
    // =============================================================================
    // Data Structures
    // =============================================================================

    /// <summary>
    /// User activity table schema
    /// </summary>
    [Table(Name = "user_activities")]
    public class UserActivity
    {
        [PrimaryKey]
        [JsonPropertyName("id")]
        public string Id { get; set; } = "";

        [JsonPropertyName("user_id")]
        public string UserId { get; set; } = "";

        [JsonPropertyName("action")]
        public string Action { get; set; } = "";

        [JsonPropertyName("timestamp")]
        public ulong Timestamp { get; set; }

        [JsonPropertyName("details")]
        public string Details { get; set; } = "";

        [JsonPropertyName("metadata")]
        public Dictionary<string, object> Metadata { get; set; } = new();
    }

    /// <summary>
    /// Activity event for real-time subscriptions
    /// </summary>
    [Event(Name = "activity_events")]
    public class ActivityEvent
    {
        [JsonPropertyName("activity_id")]
        public string ActivityId { get; set; } = "";

        [JsonPropertyName("user_id")]
        public string UserId { get; set; } = "";

        [JsonPropertyName("action")]
        public string Action { get; set; } = "";

        [JsonPropertyName("timestamp")]
        public ulong Timestamp { get; set; }

        [JsonPropertyName("event_type")]
        public string EventType { get; set; } = "";
    }

    /// <summary>
    /// User profile schema
    /// </summary>
    [Table(Name = "users")]
    public class User
    {
        [PrimaryKey]
        [JsonPropertyName("id")]
        public string Id { get; set; } = "";

        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        [JsonPropertyName("premium")]
        public bool Premium { get; set; }

        [JsonPropertyName("reputation")]
        public int Reputation { get; set; }

        [JsonPropertyName("created_at")]
        public ulong CreatedAt { get; set; }
    }

    // =============================================================================
    // Subscription Module Implementation
    // =============================================================================

    /// <summary>
    /// Main subscription module class with filters and transforms
    /// </summary>
    public class SubscriptionModuleImpl : ModuleBase
    {
        // =============================================================================
        // Subscription Functions
        // =============================================================================

        /// <summary>
        /// Filter function for recent activities (last hour only)
        /// </summary>
        [SubscriptionFilter(Name = "filter_recent_activities")]
        public static bool FilterRecentActivities(byte[] eventData)
        {
            try
            {
                var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
                var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

                // Skip DELETE events
                if (!eventObj.TryGetProperty("new_value", out var newValueElement))
                {
                    return false;
                }

                var activityJson = newValueElement.GetString();
                if (string.IsNullOrEmpty(activityJson))
                {
                    return false;
                }

                var activity = JsonSerializer.Deserialize<UserActivity>(activityJson);
                var currentTime = Utils.Now();
                var oneHourMs = 60 * 60 * 1000UL;

                // Only include activities from the last hour
                return (currentTime - activity.Timestamp) <= oneHourMs;
            }
            catch (Exception ex)
            {
                Utils.LogError($"Filter error: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// Filter function for premium users only
        /// </summary>
        [SubscriptionFilter(Name = "filter_premium_users")]
        public static bool FilterPremiumUsers(byte[] eventData)
        {
            try
            {
                var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
                var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

                if (!eventObj.TryGetProperty("new_value", out var newValueElement))
                {
                    return false;
                }

                var activityJson = newValueElement.GetString();
                if (string.IsNullOrEmpty(activityJson))
                {
                    return false;
                }

                var activity = JsonSerializer.Deserialize<UserActivity>(activityJson);

                // Check if user has premium account
                var userResult = DB.Read<User>(new Key(activity.UserId));
                if (userResult.IsErr || userResult.Unwrap() == null)
                {
                    return false;
                }

                var user = userResult.Unwrap();
                return user.Premium;
            }
            catch (Exception ex)
            {
                Utils.LogError($"Premium filter error: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// Transform function to add metadata and computed fields
        /// </summary>
        [SubscriptionTransform(Name = "transform_add_metadata")]
        public static byte[] TransformAddMetadata(byte[] eventData)
        {
            try
            {
                var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
                var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

                if (eventObj.TryGetProperty("new_value", out var newValueElement))
                {
                    var activityJson = newValueElement.GetString();
                    if (!string.IsNullOrEmpty(activityJson))
                    {
                        var activity = JsonSerializer.Deserialize<UserActivity>(activityJson);

                        // Add computed metadata
                        activity.Metadata["processed_at"] = Utils.Now();
                        activity.Metadata["server_version"] = "1.0.0";

                        // Add user reputation score
                        var userResult = DB.Read<User>(new Key(activity.UserId));
                        if (userResult.IsOk && userResult.Unwrap() != null)
                        {
                            var user = userResult.Unwrap();
                            activity.Metadata["user_reputation"] = user.Reputation;
                            activity.Metadata["user_premium"] = user.Premium;
                        }

                        // Update the event with transformed data
                        var transformedJson = JsonSerializer.Serialize(activity);
                        var newEventObj = JsonSerializer.Deserialize<Dictionary<string, object>>(eventJson);
                        newEventObj["new_value"] = transformedJson;

                        var resultJson = JsonSerializer.Serialize(newEventObj);
                        return System.Text.Encoding.UTF8.GetBytes(resultJson);
                    }
                }

                return eventData; // Return original if no transformation needed
            }
            catch (Exception ex)
            {
                Utils.LogError($"Transform error: {ex.Message}");
                return eventData; // Return original on error
            }
        }

        /// <summary>
        /// Transform function to anonymize sensitive data
        /// </summary>
        [SubscriptionTransform(Name = "transform_anonymize")]
        public static byte[] TransformAnonymize(byte[] eventData)
        {
            try
            {
                var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
                var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

                if (eventObj.TryGetProperty("new_value", out var newValueElement))
                {
                    var activityJson = newValueElement.GetString();
                    if (!string.IsNullOrEmpty(activityJson))
                    {
                        var activity = JsonSerializer.Deserialize<UserActivity>(activityJson);

                        // Replace user_id with anonymous hash
                        var hash = activity.UserId.GetHashCode();
                        activity.UserId = $"anon_{Math.Abs(hash % 100000)}";

                        // Remove potentially sensitive details
                        activity.Details = "[REDACTED]";

                        // Clear sensitive metadata
                        activity.Metadata.Clear();
                        activity.Metadata["anonymized"] = true;

                        var transformedJson = JsonSerializer.Serialize(activity);
                        var newEventObj = JsonSerializer.Deserialize<Dictionary<string, object>>(eventJson);
                        newEventObj["new_value"] = transformedJson;

                        var resultJson = JsonSerializer.Serialize(newEventObj);
                        return System.Text.Encoding.UTF8.GetBytes(resultJson);
                    }
                }

                return eventData;
            }
            catch (Exception ex)
            {
                Utils.LogError($"Anonymize error: {ex.Message}");
                return eventData;
            }
        }

        /// <summary>
        /// Get initial data for activity subscriptions
        /// </summary>
        [InitialData(Name = "get_initial_activities")]
        public static byte[] GetInitialActivities(string whereClause)
        {
            try
            {
                var initialData = new List<UserActivity>();

                // For demo purposes, create some sample activities
                // In a real implementation, you'd query the database based on the whereClause

                var activity1 = new UserActivity
                {
                    Id = "activity_1",
                    UserId = "user_123",
                    Action = "login",
                    Timestamp = Utils.Now() - 30000, // 30 seconds ago
                    Details = "User logged in from mobile app"
                };

                var activity2 = new UserActivity
                {
                    Id = "activity_2",
                    UserId = "user_456",
                    Action = "purchase",
                    Timestamp = Utils.Now() - 60000, // 1 minute ago
                    Details = "Purchased premium subscription"
                };

                initialData.Add(activity1);
                initialData.Add(activity2);

                var resultJson = JsonSerializer.Serialize(initialData);
                return System.Text.Encoding.UTF8.GetBytes(resultJson);
            }
            catch (Exception ex)
            {
                Utils.LogError($"Initial data error: {ex.Message}");
                return Array.Empty<byte>();
            }
        }

        // =============================================================================
        // Reducer Functions
        // =============================================================================

        /// <summary>
        /// Create a new user activity record
        /// </summary>
        [Reducer]
        public static int CreateActivity(ReducerContext ctx, string userId, string action, string details)
        {
            try
            {
                // Validate inputs
                if (string.IsNullOrWhiteSpace(userId))
                {
                    Utils.LogError("User ID cannot be empty");
                    return -1;
                }

                if (string.IsNullOrWhiteSpace(action))
                {
                    Utils.LogError("Action cannot be empty");
                    return -1;
                }

                var activity = new UserActivity
                {
                    Id = $"{userId}_{Utils.Now()}",
                    UserId = userId,
                    Action = action,
                    Timestamp = Utils.Now(),
                    Details = details ?? ""
                };

                // Insert into database (automatically triggers changefeed)
                ctx.Db.GetTable<UserActivity>().Insert(activity);

                // Note: Changefeed events are now automatically emitted by the database layer

                Utils.LogInfo($"Created activity: {activity.Id} ({action})");
                return 1;
            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in CreateActivity: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Get user activities with filtering
        /// </summary>
        [Reducer]
        public static int GetUserActivities(ReducerContext ctx, string userId, long sinceTimestamp)
        {
            try
            {
                Utils.LogInfo($"Getting activities for user: {userId} since: {sinceTimestamp}");

                // In a real implementation, you'd scan the activities table
                // and filter by user_id and timestamp
                // For now, just return success
                return 1;
            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in GetUserActivities: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Create a test user for demo purposes
        /// </summary>
        [Reducer]
        public static int CreateTestUser(ReducerContext ctx, string userId, string name, bool premium, int reputation)
        {
            try
            {
                var user = new User
                {
                    Id = userId,
                    Name = name,
                    Premium = premium,
                    Reputation = reputation,
                    CreatedAt = Utils.Now()
                };

                // Insert into database (automatically triggers changefeed)
                ctx.Db.GetTable<User>().Insert(user);

                Utils.LogInfo($"Created test user: {userId} ({name})");
                return 1;
            }
            catch (Exception ex)
            {
                Utils.LogError($"Exception in CreateTestUser: {ex.Message}");
                return -1;
            }
        }

        // =============================================================================
        // Module Lifecycle Overrides
        // =============================================================================

        public override int Initialize()
        {
            Utils.LogInfo("Subscription module initialized successfully");

            // Note: In the new API, initialization would use a system context
            // For now, initialization is simpler without complex database operations

            return 0;
        }

        public override int OnClientConnected(string connectionId)
        {
            Utils.LogInfo($"Client connected to subscription module: {connectionId}");

            // Note: Client lifecycle events could trigger reducer calls with system context
            // For now, keep these simple without database operations

            return 0;
        }

        public override int OnClientDisconnected(string connectionId)
        {
            Utils.LogInfo($"Client disconnected from subscription module: {connectionId}");

            // Note: Client lifecycle events could trigger reducer calls with system context
            // For now, keep these simple without database operations

            return 0;
        }
    }
}