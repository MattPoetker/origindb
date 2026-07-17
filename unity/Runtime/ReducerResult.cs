#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using UnityEngine;

namespace OriginDB.Client
{
    /// <summary>
    /// Result returned from executing a reducer on the OriginDB server.
    /// </summary>
    [Serializable]
    public class ReducerResult
    {
        [SerializeField] private bool success;
        [SerializeField] private string errorMessage;
        [SerializeField] private string data;
        [SerializeField] private Dictionary<string, object> resultData;

        /// <summary>
        /// Whether the reducer execution was successful.
        /// </summary>
        public bool Success => success;

        /// <summary>
        /// Error message if the reducer failed.
        /// </summary>
        public string ErrorMessage => errorMessage;

        /// <summary>
        /// Raw data returned from the reducer as JSON string.
        /// </summary>
        public string Data => data;

        /// <summary>
        /// Parsed result data as a dictionary.
        /// </summary>
        public Dictionary<string, object> ResultData => resultData ?? new Dictionary<string, object>();

        public ReducerResult()
        {
            success = false;
            errorMessage = "";
            data = "{}";
            resultData = new Dictionary<string, object>();
        }

        public ReducerResult(bool success, string errorMessage = null, string data = null)
        {
            this.success = success;
            this.errorMessage = errorMessage ?? "";
            this.data = data ?? "{}";
            this.resultData = new Dictionary<string, object>();

            if (success && !string.IsNullOrEmpty(data))
            {
                try
                {
                    ParseData();
                }
                catch (Exception ex)
                {
                    Debug.LogWarning($"[ReducerResult] Failed to parse result data: {ex.Message}");
                }
            }
        }

        /// <summary>
        /// Creates a successful result with data.
        /// </summary>
        public static ReducerResult CreateSuccess(string data = null)
        {
            return new ReducerResult(true, null, data);
        }

        /// <summary>
        /// Creates a successful result with structured data.
        /// </summary>
        public static ReducerResult CreateSuccess(Dictionary<string, object> data)
        {
            var result = new ReducerResult(true);
            result.resultData = data ?? new Dictionary<string, object>();
            result.data = JsonUtility.ToJson(result.resultData);
            return result;
        }

        /// <summary>
        /// Creates a failed result with an error message.
        /// </summary>
        public static ReducerResult CreateError(string errorMessage)
        {
            return new ReducerResult(false, errorMessage);
        }

        /// <summary>
        /// Gets a value from the result data by key.
        /// </summary>
        public T GetValue<T>(string key)
        {
            if (resultData != null && resultData.TryGetValue(key, out var value))
            {
                try
                {
                    if (value is T directValue)
                    {
                        return directValue;
                    }

                    // Try to convert the value
                    return (T)Convert.ChangeType(value, typeof(T));
                }
                catch (Exception ex)
                {
                    Debug.LogWarning($"[ReducerResult] Failed to convert value '{value}' to type {typeof(T)}: {ex.Message}");
                }
            }

            return default(T);
        }

        /// <summary>
        /// Checks if the result contains a specific key.
        /// </summary>
        public bool HasValue(string key)
        {
            return resultData != null && resultData.ContainsKey(key);
        }

        /// <summary>
        /// Gets all keys in the result data.
        /// </summary>
        public string[] GetKeys()
        {
            if (resultData == null)
                return new string[0];

            var keys = new string[resultData.Count];
            resultData.Keys.CopyTo(keys, 0);
            return keys;
        }

        /// <summary>
        /// Gets the result data as the specified type.
        /// </summary>
        public T GetData<T>() where T : class
        {
            if (string.IsNullOrEmpty(data) || data == "{}")
                return null;

            try
            {
                return JsonUtility.FromJson<T>(data);
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"[ReducerResult] Failed to deserialize data to {typeof(T)}: {ex.Message}");
                return null;
            }
        }

        /// <summary>
        /// Parses the JSON data into the result dictionary.
        /// </summary>
        private void ParseData()
        {
            if (string.IsNullOrEmpty(data) || data == "{}")
                return;

            try
            {
                // Simple JSON parsing for basic types
                // Note: This is a simplified parser. For complex JSON, consider using Newtonsoft.Json
                var jsonObject = JsonUtility.FromJson<Dictionary<string, object>>(data);
                if (jsonObject != null)
                {
                    resultData = jsonObject;
                }
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"[ReducerResult] Failed to parse JSON data: {ex.Message}");
                resultData = new Dictionary<string, object>();
            }
        }

        public override string ToString()
        {
            if (success)
            {
                return $"ReducerResult(Success=true, DataKeys=[{string.Join(", ", GetKeys())}])";
            }
            else
            {
                return $"ReducerResult(Success=false, Error='{errorMessage}')";
            }
        }
    }

    /// <summary>
    /// Result returned from executing a SQL query on the OriginDB server.
    /// </summary>
    [Serializable]
    public class QueryResult
    {
        [SerializeField] private bool success;
        [SerializeField] private string errorMessage;
        [SerializeField] private string data;
        [SerializeField] private int rowCount;

        /// <summary>
        /// Whether the query execution was successful.
        /// </summary>
        public bool Success => success;

        /// <summary>
        /// Error message if the query failed.
        /// </summary>
        public string ErrorMessage => errorMessage;

        /// <summary>
        /// Raw data returned from the query as JSON string.
        /// </summary>
        public string Data => data;

        /// <summary>
        /// Number of rows returned by the query.
        /// </summary>
        public int RowCount => rowCount;

        public QueryResult()
        {
            success = false;
            errorMessage = "";
            data = "[]";
            rowCount = 0;
        }

        public QueryResult(bool success, string errorMessage = null, string data = null, int rowCount = 0)
        {
            this.success = success;
            this.errorMessage = errorMessage ?? "";
            this.data = data ?? "[]";
            this.rowCount = rowCount;
        }

        /// <summary>
        /// Creates a successful query result.
        /// </summary>
        public static QueryResult CreateSuccess(string data, int rowCount = 0)
        {
            return new QueryResult(true, null, data, rowCount);
        }

        /// <summary>
        /// Creates a failed query result.
        /// </summary>
        public static QueryResult CreateError(string errorMessage)
        {
            return new QueryResult(false, errorMessage);
        }

        /// <summary>
        /// Gets the query results as a list of the specified type.
        /// </summary>
        public List<T> GetResults<T>() where T : class
        {
            if (!success || string.IsNullOrEmpty(data) || data == "[]")
                return new List<T>();

            try
            {
                // Parse JSON array into list of objects
                var wrapper = JsonUtility.FromJson<Wrapper<T>>(data);
                return wrapper?.items ?? new List<T>();
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"[QueryResult] Failed to deserialize results to {typeof(T)}: {ex.Message}");
                return new List<T>();
            }
        }

        /// <summary>
        /// Gets the first result as the specified type.
        /// </summary>
        public T GetFirstResult<T>() where T : class
        {
            var results = GetResults<T>();
            return results.Count > 0 ? results[0] : null;
        }

        public override string ToString()
        {
            if (success)
            {
                return $"QueryResult(Success=true, RowCount={rowCount})";
            }
            else
            {
                return $"QueryResult(Success=false, Error='{errorMessage}')";
            }
        }

        [Serializable]
        private class Wrapper<T>
        {
            public List<T> items;
        }
    }
}
#endif