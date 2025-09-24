#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;

namespace InstantDB.Client
{
    /// <summary>
    /// Game session data model for InstantDB integration.
    /// Represents a multiplayer game session with players and game state.
    /// </summary>
    [Serializable]
    public class GameSession
    {
        /// <summary>
        /// Unique session identifier
        /// </summary>
        public int Id { get; set; }

        /// <summary>
        /// Session display name
        /// </summary>
        public string Name { get; set; } = "";

        /// <summary>
        /// Session description
        /// </summary>
        public string Description { get; set; } = "";

        /// <summary>
        /// Maximum number of players allowed
        /// </summary>
        public int MaxPlayers { get; set; } = 8;

        /// <summary>
        /// Current number of players in session
        /// </summary>
        public int CurrentPlayers { get; set; } = 0;

        /// <summary>
        /// Session status (waiting, active, completed, etc.)
        /// </summary>
        public string Status { get; set; } = "waiting";

        /// <summary>
        /// Game mode or type
        /// </summary>
        public string GameMode { get; set; } = "default";

        /// <summary>
        /// Map or level name
        /// </summary>
        public string MapName { get; set; } = "";

        /// <summary>
        /// Whether the session is private (requires invitation)
        /// </summary>
        public bool IsPrivate { get; set; } = false;

        /// <summary>
        /// Session password (optional)
        /// </summary>
        public string Password { get; set; } = "";

        /// <summary>
        /// Host player ID
        /// </summary>
        public int HostPlayerId { get; set; } = -1;

        /// <summary>
        /// Session creation timestamp
        /// </summary>
        public DateTime CreatedAt { get; set; } = DateTime.UtcNow;

        /// <summary>
        /// Session start timestamp
        /// </summary>
        public DateTime? StartedAt { get; set; }

        /// <summary>
        /// Session end timestamp
        /// </summary>
        public DateTime? EndedAt { get; set; }

        /// <summary>
        /// Session duration in seconds (if completed)
        /// </summary>
        public int DurationSeconds { get; set; } = 0;

        /// <summary>
        /// Custom session configuration as JSON string
        /// </summary>
        public string Config { get; set; } = "{}";

        /// <summary>
        /// Session tags for filtering and searching
        /// </summary>
        public string Tags { get; set; } = "";

        /// <summary>
        /// Session region or server location
        /// </summary>
        public string Region { get; set; } = "";

        /// <summary>
        /// Last update timestamp
        /// </summary>
        public DateTime LastUpdated { get; set; } = DateTime.UtcNow;

        public GameSession()
        {
            CreatedAt = DateTime.UtcNow;
            LastUpdated = DateTime.UtcNow;
        }

        public GameSession(string name, int maxPlayers = 8)
        {
            Name = name;
            MaxPlayers = maxPlayers;
            CreatedAt = DateTime.UtcNow;
            LastUpdated = DateTime.UtcNow;
        }

        /// <summary>
        /// Checks if the session is full (at max player capacity).
        /// </summary>
        public bool IsFull => CurrentPlayers >= MaxPlayers;

        /// <summary>
        /// Checks if the session can accept new players.
        /// </summary>
        public bool CanJoin => !IsFull && (Status == "waiting" || Status == "active") && !IsEnded;

        /// <summary>
        /// Checks if the session is currently active/in progress.
        /// </summary>
        public bool IsActive => Status == "active" && !IsEnded;

        /// <summary>
        /// Checks if the session has ended.
        /// </summary>
        public bool IsEnded => EndedAt.HasValue || Status == "completed" || Status == "ended";

        /// <summary>
        /// Gets the session duration in a readable format.
        /// </summary>
        public TimeSpan Duration
        {
            get
            {
                if (EndedAt.HasValue && StartedAt.HasValue)
                {
                    return EndedAt.Value - StartedAt.Value;
                }
                else if (StartedAt.HasValue && !EndedAt.HasValue)
                {
                    return DateTime.UtcNow - StartedAt.Value;
                }
                return TimeSpan.Zero;
            }
        }

        /// <summary>
        /// Gets available player slots.
        /// </summary>
        public int AvailableSlots => Math.Max(0, MaxPlayers - CurrentPlayers);

        /// <summary>
        /// Starts the session (sets status to active and records start time).
        /// </summary>
        public GameSession Start()
        {
            var copy = Clone();
            copy.Status = "active";
            copy.StartedAt = DateTime.UtcNow;
            copy.LastUpdated = DateTime.UtcNow;
            return copy;
        }

        /// <summary>
        /// Ends the session (sets status to completed and records end time).
        /// </summary>
        public GameSession End()
        {
            var copy = Clone();
            copy.Status = "completed";
            copy.EndedAt = DateTime.UtcNow;
            copy.LastUpdated = DateTime.UtcNow;

            if (StartedAt.HasValue)
            {
                copy.DurationSeconds = (int)(copy.EndedAt.Value - StartedAt.Value).TotalSeconds;
            }

            return copy;
        }

        /// <summary>
        /// Updates the current player count.
        /// </summary>
        public GameSession WithPlayerCount(int playerCount)
        {
            var copy = Clone();
            copy.CurrentPlayers = Math.Max(0, Math.Min(playerCount, MaxPlayers));
            copy.LastUpdated = DateTime.UtcNow;
            return copy;
        }

        /// <summary>
        /// Creates a copy of this session.
        /// </summary>
        public GameSession Clone()
        {
            return new GameSession
            {
                Id = Id,
                Name = Name,
                Description = Description,
                MaxPlayers = MaxPlayers,
                CurrentPlayers = CurrentPlayers,
                Status = Status,
                GameMode = GameMode,
                MapName = MapName,
                IsPrivate = IsPrivate,
                Password = Password,
                HostPlayerId = HostPlayerId,
                CreatedAt = CreatedAt,
                StartedAt = StartedAt,
                EndedAt = EndedAt,
                DurationSeconds = DurationSeconds,
                Config = Config,
                Tags = Tags,
                Region = Region,
                LastUpdated = LastUpdated
            };
        }

        /// <summary>
        /// Gets session tags as a list.
        /// </summary>
        public List<string> GetTags()
        {
            if (string.IsNullOrEmpty(Tags))
                return new List<string>();

            return new List<string>(Tags.Split(','));
        }

        /// <summary>
        /// Sets session tags from a list.
        /// </summary>
        public void SetTags(List<string> tags)
        {
            Tags = tags != null ? string.Join(",", tags) : "";
            LastUpdated = DateTime.UtcNow;
        }

        public override string ToString()
        {
            return $"GameSession(Id={Id}, Name='{Name}', Players={CurrentPlayers}/{MaxPlayers}, Status='{Status}', GameMode='{GameMode}')";
        }

        public override bool Equals(object obj)
        {
            if (obj is GameSession other)
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