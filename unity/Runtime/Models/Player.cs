#if UNITY_5_3_OR_NEWER
using UnityEngine;
using System;

namespace OriginDB.Client
{
    /// <summary>
    /// Player data model for OriginDB integration.
    /// Represents a player in the game with position, health, and other game state.
    /// </summary>
    [Serializable]
    public class Player
    {
        /// <summary>
        /// Unique player identifier
        /// </summary>
        public int Id { get; set; }

        /// <summary>
        /// Player display name
        /// </summary>
        public string Name { get; set; } = "";

        /// <summary>
        /// Player position in world space
        /// </summary>
        public Vector3 Position { get; set; } = Vector3.zero;

        /// <summary>
        /// Player rotation
        /// </summary>
        public Quaternion Rotation { get; set; } = Quaternion.identity;

        /// <summary>
        /// Player health points
        /// </summary>
        public int Health { get; set; } = 100;

        /// <summary>
        /// Maximum health points
        /// </summary>
        public int MaxHealth { get; set; } = 100;

        /// <summary>
        /// Player score/points
        /// </summary>
        public int Score { get; set; } = 0;

        /// <summary>
        /// Player team identifier
        /// </summary>
        public string Team { get; set; } = "";

        /// <summary>
        /// Whether the player is currently active/alive
        /// </summary>
        public bool IsActive { get; set; } = true;

        /// <summary>
        /// Player level or experience level
        /// </summary>
        public int Level { get; set; } = 1;

        /// <summary>
        /// Player color for visual representation
        /// </summary>
        public Color PlayerColor { get; set; } = Color.white;

        /// <summary>
        /// Custom player data as JSON string
        /// </summary>
        public string CustomData { get; set; } = "{}";

        /// <summary>
        /// Last update timestamp
        /// </summary>
        public DateTime LastUpdated { get; set; } = DateTime.UtcNow;

        /// <summary>
        /// Session ID this player belongs to
        /// </summary>
        public int SessionId { get; set; } = -1;

        public Player()
        {
            LastUpdated = DateTime.UtcNow;
        }

        public Player(int id, string name)
        {
            Id = id;
            Name = name;
            LastUpdated = DateTime.UtcNow;
        }

        /// <summary>
        /// Creates a copy of this player with updated position.
        /// </summary>
        public Player WithPosition(Vector3 newPosition)
        {
            var copy = Clone();
            copy.Position = newPosition;
            copy.LastUpdated = DateTime.UtcNow;
            return copy;
        }

        /// <summary>
        /// Creates a copy of this player with updated health.
        /// </summary>
        public Player WithHealth(int newHealth)
        {
            var copy = Clone();
            copy.Health = Mathf.Clamp(newHealth, 0, MaxHealth);
            copy.LastUpdated = DateTime.UtcNow;
            return copy;
        }

        /// <summary>
        /// Creates a copy of this player.
        /// </summary>
        public Player Clone()
        {
            return new Player
            {
                Id = Id,
                Name = Name,
                Position = Position,
                Rotation = Rotation,
                Health = Health,
                MaxHealth = MaxHealth,
                Score = Score,
                Team = Team,
                IsActive = IsActive,
                Level = Level,
                PlayerColor = PlayerColor,
                CustomData = CustomData,
                LastUpdated = LastUpdated,
                SessionId = SessionId
            };
        }

        /// <summary>
        /// Checks if this player is alive (health > 0 and active).
        /// </summary>
        public bool IsAlive => Health > 0 && IsActive;

        /// <summary>
        /// Gets health as a percentage (0.0 to 1.0).
        /// </summary>
        public float HealthPercentage => MaxHealth > 0 ? (float)Health / MaxHealth : 0f;

        public override string ToString()
        {
            return $"Player(Id={Id}, Name='{Name}', Position={Position}, Health={Health}/{MaxHealth}, Team='{Team}')";
        }

        public override bool Equals(object obj)
        {
            if (obj is Player other)
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