package no.nordicsemi.android.meshprovisioner.transport;

import android.support.annotation.NonNull;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import no.nordicsemi.android.meshprovisioner.opcodes.ApplicationMessageOpCodes;
import no.nordicsemi.android.meshprovisioner.utils.SecureUtils;

/**
 * To be used as a wrapper class when creating a SceneStoreUnacknowledged message.
 */
@SuppressWarnings("unused")
public class SceneStoreUnacknowledged extends GenericMessage {

    private static final String TAG = SceneStoreUnacknowledged.class.getSimpleName();
    private static final int OP_CODE = ApplicationMessageOpCodes.SCENE_STORE_UNACKNOWLEDGED;
    private static final int SCENE_STORE_PARAMS_LENGTH = 2;

    private final int mSceneNumber;

    /**
     * Constructs SceneStoreUnacknowledged message.
     *
     * @param appKey      application key for this message
     * @param sceneNumber scene number of SceneStoreUnacknowledged message
     * @throws IllegalArgumentException if any illegal arguments are passed
     */
    @SuppressWarnings("WeakerAccess")
    public SceneStoreUnacknowledged(@NonNull final byte[] appKey,
                                    final int sceneNumber) {
        super(appKey);
        this.mSceneNumber = sceneNumber;
        assembleMessageParameters();
    }

    @Override
    public int getOpCode() {
        return OP_CODE;
    }

    @Override
    void assembleMessageParameters() {
        mAid = SecureUtils.calculateK4(mAppKey);
        final ByteBuffer paramsBuffer;
        Log.v(TAG, "State Number: " + mSceneNumber);
        paramsBuffer = ByteBuffer.allocate(SCENE_STORE_PARAMS_LENGTH).order(ByteOrder.LITTLE_ENDIAN);
        paramsBuffer.putShort((short) mSceneNumber);
        mParameters = paramsBuffer.array();
    }
}
