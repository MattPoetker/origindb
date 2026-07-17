#if UNITY_EDITOR
using UnityEngine;
using UnityEditor;
using OriginDB.Unity;

namespace OriginDB.Unity.Editor
{
    /// <summary>
    /// Custom property drawer for OriginDBConfig to provide better Inspector experience.
    /// </summary>
    [CustomEditor(typeof(OriginDBConfig))]
    public class OriginDBConfigEditor : UnityEditor.Editor
    {
        private SerializedProperty serverUrlProp;
        private SerializedProperty moduleNameProp;
        private SerializedProperty autoConnectProp;
        private SerializedProperty autoReconnectProp;
        private SerializedProperty maxReconnectAttemptsProp;
        private SerializedProperty reconnectIntervalProp;
        private SerializedProperty connectionTimeoutProp;
        private SerializedProperty requestTimeoutProp;
        private SerializedProperty authTokenProp;
        private SerializedProperty enableCompressionProp;
        private SerializedProperty maxMessageQueueProp;
        private SerializedProperty enableDebugLoggingProp;
        private SerializedProperty logConnectionEventsProp;
        private SerializedProperty logMessagesProp;
        private SerializedProperty environmentProfilesProp;
        private SerializedProperty activeProfileIndexProp;

        private bool showAdvancedSettings = false;
        private bool showEnvironmentProfiles = true;
        private bool showDebugSettings = false;

        private void OnEnable()
        {
            serverUrlProp = serializedObject.FindProperty("serverUrl");
            moduleNameProp = serializedObject.FindProperty("moduleName");
            autoConnectProp = serializedObject.FindProperty("autoConnect");
            autoReconnectProp = serializedObject.FindProperty("autoReconnect");
            maxReconnectAttemptsProp = serializedObject.FindProperty("maxReconnectAttempts");
            reconnectIntervalProp = serializedObject.FindProperty("reconnectInterval");
            connectionTimeoutProp = serializedObject.FindProperty("connectionTimeout");
            requestTimeoutProp = serializedObject.FindProperty("requestTimeout");
            authTokenProp = serializedObject.FindProperty("authToken");
            enableCompressionProp = serializedObject.FindProperty("enableCompression");
            maxMessageQueueProp = serializedObject.FindProperty("maxMessageQueue");
            enableDebugLoggingProp = serializedObject.FindProperty("enableDebugLogging");
            logConnectionEventsProp = serializedObject.FindProperty("logConnectionEvents");
            logMessagesProp = serializedObject.FindProperty("logMessages");
            environmentProfilesProp = serializedObject.FindProperty("environmentProfiles");
            activeProfileIndexProp = serializedObject.FindProperty("activeProfileIndex");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            var config = target as OriginDBConfig;

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("OriginDB Configuration", EditorStyles.boldLabel);
            EditorGUILayout.Space();

            // Validation status
            string validationMessage;
            bool isValid = config.ValidateConfiguration(out validationMessage);

            if (!isValid)
            {
                EditorGUILayout.HelpBox($"Configuration Error: {validationMessage}", MessageType.Error);
                EditorGUILayout.Space();
            }
            else
            {
                EditorGUILayout.HelpBox("Configuration is valid", MessageType.Info);
                EditorGUILayout.Space();
            }

            // Environment Profiles Section
            showEnvironmentProfiles = EditorGUILayout.Foldout(showEnvironmentProfiles, "Environment Profiles", true);
            if (showEnvironmentProfiles)
            {
                EditorGUI.indentLevel++;

                EditorGUILayout.PropertyField(activeProfileIndexProp, new GUIContent("Active Profile"));

                if (config.environmentProfiles != null && config.environmentProfiles.Length > 0)
                {
                    var activeProfile = config.ActiveProfile;
                    if (activeProfile != null)
                    {
                        EditorGUILayout.Space();
                        EditorGUILayout.LabelField($"Current Profile: {activeProfile.name}", EditorStyles.miniLabel);
                        EditorGUILayout.LabelField($"Server URL: {activeProfile.serverUrl}", EditorStyles.miniLabel);
                        EditorGUILayout.LabelField($"Module: {activeProfile.moduleName}", EditorStyles.moduleName);
                    }
                }

                EditorGUILayout.Space();
                EditorGUILayout.PropertyField(environmentProfilesProp, new GUIContent("Profiles"), true);

                EditorGUI.indentLevel--;
                EditorGUILayout.Space();
            }

            // Basic Settings
            EditorGUILayout.LabelField("Basic Settings", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(serverUrlProp);
            EditorGUILayout.PropertyField(moduleNameProp);
            EditorGUILayout.PropertyField(autoConnectProp);
            EditorGUILayout.Space();

            // Connection Settings
            EditorGUILayout.LabelField("Connection Settings", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(autoReconnectProp);
            if (autoReconnectProp.boolValue)
            {
                EditorGUI.indentLevel++;
                EditorGUILayout.PropertyField(maxReconnectAttemptsProp);
                EditorGUILayout.PropertyField(reconnectIntervalProp, new GUIContent("Reconnect Interval (seconds)"));
                EditorGUI.indentLevel--;
            }
            EditorGUILayout.Space();

            // Advanced Settings
            showAdvancedSettings = EditorGUILayout.Foldout(showAdvancedSettings, "Advanced Settings", true);
            if (showAdvancedSettings)
            {
                EditorGUI.indentLevel++;
                EditorGUILayout.PropertyField(connectionTimeoutProp, new GUIContent("Connection Timeout (seconds)"));
                EditorGUILayout.PropertyField(requestTimeoutProp, new GUIContent("Request Timeout (seconds)"));
                EditorGUILayout.PropertyField(authTokenProp);
                EditorGUILayout.PropertyField(enableCompressionProp);
                EditorGUILayout.PropertyField(maxMessageQueueProp);
                EditorGUI.indentLevel--;
                EditorGUILayout.Space();
            }

            // Debug Settings
            showDebugSettings = EditorGUILayout.Foldout(showDebugSettings, "Debug Settings", true);
            if (showDebugSettings)
            {
                EditorGUI.indentLevel++;
                EditorGUILayout.PropertyField(enableDebugLoggingProp);
                EditorGUILayout.PropertyField(logConnectionEventsProp);
                EditorGUILayout.PropertyField(logMessagesProp);

                if (logMessagesProp.boolValue)
                {
                    EditorGUILayout.HelpBox("Warning: Message logging can impact performance and expose sensitive data. Only enable for debugging.", MessageType.Warning);
                }

                EditorGUI.indentLevel--;
                EditorGUILayout.Space();
            }

            // Utility Buttons
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Utilities", EditorStyles.boldLabel);

            GUILayout.BeginHorizontal();
            if (GUILayout.Button("Reset to Defaults"))
            {
                if (EditorUtility.DisplayDialog("Reset Configuration",
                    "This will reset all settings to their default values. Continue?",
                    "Reset", "Cancel"))
                {
                    config.ResetToDefaults();
                    EditorUtility.SetDirty(config);
                }
            }

            if (GUILayout.Button("Test Connection"))
            {
                if (Application.isPlaying)
                {
                    TestConnection(config);
                }
                else
                {
                    EditorUtility.DisplayDialog("Test Connection",
                        "Connection testing is only available in Play Mode.", "OK");
                }
            }
            GUILayout.EndHorizontal();

            if (GUILayout.Button("Create Connection Options"))
            {
                var options = config.CreateConnectionOptions();
                Debug.Log($"Connection Options: {options}");
                EditorUtility.DisplayDialog("Connection Options",
                    $"Options created successfully!\nCheck the Console for details.", "OK");
            }

            serializedObject.ApplyModifiedProperties();
        }

        private void TestConnection(OriginDBConfig config)
        {
            try
            {
                if (OriginDBNetworkManager.Instance != null)
                {
                    var connection = OriginDBNetworkManager.Instance.CreateConnection(config.CreateConnectionOptions());
                    Debug.Log($"[OriginDB Editor] Test connection created: {connection}");
                    EditorUtility.DisplayDialog("Test Connection",
                        "Test connection created successfully! Check the Console for details.", "OK");
                }
                else
                {
                    EditorUtility.DisplayDialog("Test Connection",
                        "No OriginDBNetworkManager found in the scene.", "OK");
                }
            }
            catch (System.Exception ex)
            {
                Debug.LogError($"[OriginDB Editor] Test connection failed: {ex.Message}");
                EditorUtility.DisplayDialog("Test Connection Failed",
                    $"Failed to create test connection:\n{ex.Message}", "OK");
            }
        }
    }

    /// <summary>
    /// Menu items for creating OriginDB assets and objects.
    /// </summary>
    public static class OriginDBMenuItems
    {
        [MenuItem("GameObject/OriginDB/Network Manager", false, 10)]
        public static void CreateNetworkManager(MenuCommand menuCommand)
        {
            var go = new GameObject("OriginDB Network Manager");
            go.AddComponent<OriginDBNetworkManager>();

            GameObjectUtility.SetParentAndAlign(go, menuCommand.context as GameObject);
            Undo.RegisterCreatedObjectUndo(go, "Create OriginDB Network Manager");
            Selection.activeObject = go;
        }

        [MenuItem("Assets/Create/OriginDB/Configuration")]
        public static void CreateConfiguration()
        {
            var config = ScriptableObject.CreateInstance<OriginDBConfig>();
            config.ResetToDefaults();

            string path = AssetDatabase.GetAssetPath(Selection.activeObject);
            if (path == "")
            {
                path = "Assets";
            }
            else if (System.IO.Path.GetExtension(path) != "")
            {
                path = path.Replace(System.IO.Path.GetFileName(AssetDatabase.GetAssetPath(Selection.activeObject)), "");
            }

            string assetPathAndName = AssetDatabase.GenerateUniqueAssetPath(path + "/OriginDBConfig.asset");

            AssetDatabase.CreateAsset(config, assetPathAndName);
            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
            EditorUtility.FocusProjectWindow();
            Selection.activeObject = config;
        }

        [MenuItem("Assets/Create/OriginDB/Local Development Config")]
        public static void CreateLocalConfig()
        {
            var config = OriginDBConfigUtility.CreateLocalConfig();

            string path = AssetDatabase.GetAssetPath(Selection.activeObject);
            if (path == "")
            {
                path = "Assets";
            }
            else if (System.IO.Path.GetExtension(path) != "")
            {
                path = path.Replace(System.IO.Path.GetFileName(AssetDatabase.GetAssetPath(Selection.activeObject)), "");
            }

            string assetPathAndName = AssetDatabase.GenerateUniqueAssetPath(path + "/OriginDBConfig_Local.asset");

            AssetDatabase.CreateAsset(config, assetPathAndName);
            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
            EditorUtility.FocusProjectWindow();
            Selection.activeObject = config;
        }

        [MenuItem("Assets/Create/OriginDB/Production Config")]
        public static void CreateProductionConfig()
        {
            var config = OriginDBConfigUtility.CreateProductionConfig();

            string path = AssetDatabase.GetAssetPath(Selection.activeObject);
            if (path == "")
            {
                path = "Assets";
            }
            else if (System.IO.Path.GetExtension(path) != "")
            {
                path = path.Replace(System.IO.Path.GetFileName(AssetDatabase.GetAssetPath(Selection.activeObject)), "");
            }

            string assetPathAndName = AssetDatabase.GenerateUniqueAssetPath(path + "/OriginDBConfig_Production.asset");

            AssetDatabase.CreateAsset(config, assetPathAndName);
            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
            EditorUtility.FocusProjectWindow();
            Selection.activeObject = config;
        }

        [MenuItem("OriginDB/Documentation")]
        public static void OpenDocumentation()
        {
            Application.OpenURL("https://docs.origindb.com/unity");
        }

        [MenuItem("OriginDB/Support")]
        public static void OpenSupport()
        {
            Application.OpenURL("https://github.com/your-org/origindb/issues");
        }

        [MenuItem("OriginDB/About")]
        public static void ShowAbout()
        {
            EditorUtility.DisplayDialog("OriginDB for Unity",
                "Real-time database integration for Unity\n\nVersion: 1.0.0\nWebsite: https://origindb.com", "OK");
        }
    }
}
#endif