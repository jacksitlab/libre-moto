package net.osmand.aidlapi.map;

import android.os.Bundle;
import android.os.Parcel;
import java.util.ArrayList;
import java.util.List;
import net.osmand.aidlapi.AidlParams;

public class NearbyRoadsResult extends AidlParams {

	private ArrayList<ARoadPolyline> roads = new ArrayList<>();
	private int totalFound = 0;

	public NearbyRoadsResult() {
	}

	protected NearbyRoadsResult(Parcel in) {
		readFromParcel(in);
	}

	public static final Creator<NearbyRoadsResult> CREATOR = new Creator<NearbyRoadsResult>() {
		@Override
		public NearbyRoadsResult createFromParcel(Parcel in) {
			return new NearbyRoadsResult(in);
		}

		@Override
		public NearbyRoadsResult[] newArray(int size) {
			return new NearbyRoadsResult[size];
		}
	};

	public List<ARoadPolyline> getRoads() { return roads; }
	public void setRoads(List<ARoadPolyline> r) {
		roads = new ArrayList<>();
		if (r != null) roads.addAll(r);
	}
	public int getTotalFound() { return totalFound; }
	public void setTotalFound(int t) { totalFound = t; }

	@Override
	protected void readFromBundle(Bundle bundle) {
		totalFound = bundle.getInt("totalFound");
		ArrayList<ARoadPolyline> r = bundle.getParcelableArrayList("roads");
		if (r != null) roads = r;
	}

	@Override
	public void writeToBundle(Bundle bundle) {
		bundle.putInt("totalFound", totalFound);
		bundle.putParcelableArrayList("roads", roads);
	}
}
