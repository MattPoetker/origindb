#if UNITY_5_3_OR_NEWER
using System;
using UnityEngine;

namespace InstantDB.Client
{
    /// <summary>
    /// Player action data model for InstantDB integration.
    /// Represents an action performed by a player in the game.
    /// </summary>
    [Serializable]
    public class PlayerAction
    {
        /// <summary>
        /// Unique action identifier
        /// </summary>
        public int Id { get; set; }

        /// <summary>
        /// ID of the player who performed the action
        /// </summary>
        public int PlayerId { get; set; }

        /// <summary>
        /// Session ID where the action occurred
        /// </summary>
        public int SessionId { get; set; }

        /// <summary>
        /// Type of action performed (move, shoot, jump, etc.)
        /// </summary>
        public string ActionType { get; set; } = "";

        /// <summary>
        /// Action data as JSON string
        /// </summary>
        public string Data { get; set; } = "{}";

        /// <summary>
        /// Position where the action occurred
        /// </summary>
        public Vector3 Position { get; set; } = Vector3.zero;

        /// <summary>
        /// Target position for the action (if applicable)
        /// </summary>
        public Vector3 TargetPosition { get; set; } = Vector3.zero;

        /// <summary>
        /// Target player ID (if action affects another player)
        /// </summary>
        public int TargetPlayerId { get; set; } = -1;

        /// <summary>
        /// Action timestamp
        /// </summary>
        public DateTime Timestamp { get; set; } = DateTime.UtcNow;

        /// <summary>
        /// Whether the action was successful
        /// </summary>
        public bool Success { get; set; } = true;

        /// <summary>
        /// Result or outcome of the action
        /// </summary>
        public string Result { get; set; } = "";

        /// <summary>
        /// Action priority or severity
        /// </summary>
        public int Priority { get; set; } = 0;

        public PlayerAction()
        {
            Timestamp = DateTime.UtcNow;
        }

        public PlayerAction(int playerId, string actionType)
        {
            PlayerId = playerId;
            ActionType = actionType;
            Timestamp = DateTime.UtcNow;
        }

        public PlayerAction(int playerId, string actionType, object data)
        {
            PlayerId = playerId;
            ActionType = actionType;
            Data = data != null ? JsonUtility.ToJson(data) : "{}";
            Timestamp = DateTime.UtcNow;
        }

        /// <summary>
        /// Gets the action data as the specified type.
        /// </summary>
        public T GetData<T>() where T : class
        {
            if (string.IsNullOrEmpty(Data) || Data == "{}")
                return null;

            try
            {
                return JsonUtility.FromJson<T>(Data);
            }
            catch
            {
                return null;
            }
        }

        /// <summary>
        /// Sets the action data from an object.
        /// </summary>
        public void SetData<T>(T data) where T : class
        {
            Data = data != null ? JsonUtility.ToJson(data) : "{}";
        }

        /// <summary>
        /// Creates a movement action.
        /// </summary>
        public static PlayerAction CreateMovement(int playerId, Vector3 fromPosition, Vector3 toPosition)
        {
            return new PlayerAction(playerId, "move")
            {
                Position = fromPosition,
                TargetPosition = toPosition,
                Data = JsonUtility.ToJson(new { from_x = fromPosition.x, from_y = fromPosition.y, from_z = fromPosition.z, to_x = toPosition.x, to_y = toPosition.y, to_z = toPosition.z })
            };
        }

        /// <summary>
        /// Creates a combat action.
        /// </summary>
        public static PlayerAction CreateCombat(int playerId, int targetPlayerId, string combatType, int damage = 0)
        {
            return new PlayerAction(playerId, "combat")
            {
                TargetPlayerId = targetPlayerId,
                Data = JsonUtility.ToJson(new { combat_type = combatType, damage = damage })
            };
        }

        /// <summary>
        /// Creates an interaction action.
        /// </summary>
        public static PlayerAction CreateInteraction(int playerId, string interactionType, Vector3 position)
        {
            return new PlayerAction(playerId, "interact")
            {
                Position = position,
                Data = JsonUtility.ToJson(new { interaction_type = interactionType, x = position.x, y = position.y, z = position.z })
            };
        }

        /// <summary>
        /// Creates a chat action.
        /// </summary>
        public static PlayerAction CreateChat(int playerId, string message, string channel = "global")
        {
            return new PlayerAction(playerId, "chat")
            {
                Data = JsonUtility.ToJson(new { message = message, channel = channel })
            };
        }

        /// <summary>
        /// Checks if this action is targeting another player.
        /// </summary>
        public bool HasTarget => TargetPlayerId >= 0;

        /// <summary>
        /// Checks if this action has position data.
        /// </summary>
        public bool HasPosition => Position != Vector3.zero;

        /// <summary>
        /// Checks if this action has target position data.
        /// </summary>
        public bool HasTargetPosition => TargetPosition != Vector3.zero;

        /// <summary>
        /// Gets the age of this action.
        /// </summary>
        public TimeSpan Age => DateTime.UtcNow - Timestamp;

        public override string ToString()
        {
            return $"PlayerAction(Id={Id}, PlayerId={PlayerId}, ActionType='{ActionType}', Success={Success}, Timestamp={Timestamp})";
        }

        public override bool Equals(object obj)
        {
            if (obj is PlayerAction other)
            {
                return Id == other.Id;
            }
            return false;
        }

        public override int GetHashCode()
        {
            return Id.GetHashCode();
        }
    }
}
#endif